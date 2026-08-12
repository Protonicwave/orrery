# The Python bindings

Everything the simulator does, reachable from Python, with the particle state as
NumPy arrays that share memory with the run rather than copies of it.

## Installing

From a clone:

```
pip install .
```

That builds the whole C++ library, which takes a couple of minutes, and installs
the `orrery` package with the extension inside it. The only requirements are
CMake 3.25 or later and a C++20 compiler; pip fetches the rest into an isolated
build environment.

For the example notebooks:

```
pip install ".[notebooks]"
```

## Working on the bindings

A wheel is the wrong tool for that, since every edit would mean a reinstall. The
`python` preset writes the extension into a directory that is already a working
package:

```
cmake --preset python
cmake --build --preset python
ctest --preset python
```

`build/python/python` then holds an `orrery` directory with the extension and
the package sources in it, so putting that on `PYTHONPATH` gives a working
import against the build tree:

```
PYTHONPATH=build/python/python python -c "import orrery; print(orrery.__version__)"
```

The test suite is pytest and runs under CTest with everything else, so a single
`ctest` invocation is the whole definition of done rather than most of it. It
needs NumPy and pytest; a configure that cannot find them says so and leaves the
suite unregistered rather than failing it.

## A first run

```python
import orrery

configuration = orrery.Configuration()
configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
configuration.initial_conditions.count = 4096
configuration.run.timestep = 1.0 / 64.0
configuration.run.steps = 1000
configuration.solver.kind = orrery.SolverKind.barnes_hut
configuration.solver.softening = 0.02

assert orrery.problems_with(configuration) == []

simulation = orrery.assemble(configuration)
before = simulation.measure()
simulation.run(configuration.run.steps)
after = simulation.measure()

print(f"energy error {abs((after.total_energy - before.total_energy) / before.total_energy):.3e}")
print(f"virial ratio {after.virial_ratio:.3f}")
```

`Configuration` is the same record the configuration file parses into, so a run
set up here and a run set up from a document are the same run. `write_configuration`
turns one into the other:

```python
open("cluster.orrery", "w").write(orrery.write_configuration(configuration))
```

and `orrery run cluster.orrery` then does what the script did.

## The state, without copying

This is what the bindings are for. The component arrays are NumPy views of the
storage the solver is writing into:

```python
x = simulation.particles.position_x   # a view, not a copy

for _ in range(100):
    simulation.step()
    # x reports where the particles are now. Nothing was copied to find out.
```

Positions are held as three contiguous arrays rather than as one array of
triples, because that is what makes the force kernel fast (ADR-0004), so an
`(N, 3)` array of them exists nowhere in memory. The interface says so:

| What you want | How to get it | Cost |
| --- | --- | --- |
| One component | `particles.position_x` | A view. No copy |
| All three | `orrery.components(particles)` | Three views in a tuple. No copy |
| An `(N, 3)` array | `orrery.stacked(particles)` | **A copy** |
| Distances from the origin | `orrery.radii(particles)` | A copy of one array |

The ten arrays are `position_x`, `position_y`, `position_z`, `velocity_x`,
`velocity_y`, `velocity_z`, `acceleration_x`, `acceleration_y`,
`acceleration_z` and `mass`. ADR-0040 records why there is no `(N, 3)` property.

### Writing

A `ParticleData` you built is writable, and assigning into its arrays sets up a
configuration:

```python
pair = orrery.ParticleData(2)
pair.position_x[:] = [-0.5, 0.5]
pair.velocity_y[:] = [-0.5, 0.5]
pair.mass[:] = 1.0
```

A running simulation's state is not. `simulation.particles` is a `ParticleView`
whose arrays have NumPy's `writeable` flag cleared, because the integrators
require the acceleration array to hold the acceleration at the current positions
on entry to every step and writing into a live run would break that silently.
To change a run's state, build the state you want and hand it over:

```python
state = simulation.particles.copy()
state.velocity_x[:] *= 1.01
simulation.restore(state, step=simulation.step_index)
```

`restore` re-establishes the invariant, at the cost of one force evaluation.
ADR-0041 records the decision.

### Lifetimes

A view keeps the object that owns its storage alive, so an array outlives the
expression that produced it and outlives the last Python reference to the store.
It does not survive a reallocation: `resize`, `reserve`, `add` and
`Simulation.restore` may move the component arrays, and a view taken before one
of those points at freed memory afterwards, exactly as a `std::span` would. Take
the view again after any of them.

## Comparing solvers

`compute_accelerations` evaluates the forces on a set of particles once, in
place, with whichever solver the configuration names, and returns the work it
took. It is how an approximation is measured against the reference:

```python
import numpy as np

particles = orrery.plummer_sphere(orrery.PlummerParameters(count=8192), seed=1)

def field(kind, opening_angle=0.5):
    configuration = orrery.Configuration()
    configuration.solver.kind = kind
    configuration.solver.opening_angle = opening_angle
    configuration.solver.softening = 0.02

    data = particles.copy()
    count = orrery.compute_accelerations(configuration, data)
    return orrery.stacked(data, "acceleration"), count

exact, _ = field(orrery.SolverKind.direct)
approximate, count = field(orrery.SolverKind.barnes_hut)

error = np.sqrt(np.mean(np.sum((approximate - exact) ** 2, axis=1)))
print(f"{error:.3e} against {count.particle_particle + count.particle_cell:,} interactions")
```

The interaction counter reports what the algorithm did rather than how long the
machine took, which is what makes that comparison a statement about the methods.

## Threads and the interpreter lock

`Simulation.step`, `Simulation.run`, `Simulation.measure` and
`compute_accelerations` release the interpreter lock for the duration of the
work, so the solver's own threads have the machine to themselves and another
Python thread can watch a run while it happens.

`assemble` releases it for the sampling and the first force evaluation, which is
where its time goes.

## Precision

`orrery.dtype` is the NumPy scalar type the library was built with, `float64` by
default and `float32` in a single-precision build. A tolerance in a script
should be written in terms of it rather than assumed, since one written for
double precision silently asserts nothing when the same code runs against a
single-precision build.

```python
if orrery.single_precision:
    tolerance = 1e-5
else:
    tolerance = 1e-10
```

## The example notebooks

In [`python/notebooks/`](../python/notebooks). They are committed without
outputs, so running one is the only way to see its figures, and every claim they
make is asserted before it is plotted.

| Notebook | What it shows |
| --- | --- |
| `01_validation.ipynb` | The measured convergence order of each integrator, bounded against secular energy error over four hundred orbits, and a sampled Plummer sphere against the closed-form model |
| `02_solver_accuracy.ipynb` | Barnes-Hut against direct summation: error against opening angle, error against cost, the interaction saving as N grows, and the momentum conservation a tree gives up |
| `03_galaxy_collision.ipynb` | The demonstration scenario, run and plotted, with the energy it conserved and where the material ended up |

Run them from a clean environment with:

```
pip install ".[notebooks]"
jupyter lab python/notebooks
```

or execute them without opening anything:

```
jupyter nbconvert --to notebook --execute python/notebooks/01_validation.ipynb --stdout > /dev/null
```

The numbers they produce are the numbers the C++ test suite asserts. The
measured convergence orders come out at 1.9998, 4.0006 and 4.1659 against stated
orders of 2, 4 and 4, which are the figures in the README, taken by a different
route.

## What is not bound

The trajectory and checkpoint readers, and the renderer. Neither is an
oversight.

A run driven from Python reports through the NumPy views of its own state, which
is strictly more than a binary file it would have to parse afterwards, so
`assemble` attaches no output and the `output` section of a configuration is
ignored. The command-line program is what writes trajectories and checkpoints,
and the formats are specified in [`docs/formats/`](formats/) well enough to be
read by something other than this program.

The renderer needs a window, an OpenGL context and a display, none of which
belongs in an extension module. `orrery-view` is what watches a run.
