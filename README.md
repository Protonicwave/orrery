# Orrery

[![CI](https://github.com/Protonicwave/orrery/actions/workflows/ci.yml/badge.svg)](https://github.com/Protonicwave/orrery/actions/workflows/ci.yml)
[![Documentation](https://github.com/Protonicwave/orrery/actions/workflows/docs.yml/badge.svg)](https://protonicwave.github.io/orrery/)
[![PyPI](https://img.shields.io/pypi/v/orrery-nbody)](https://pypi.org/project/orrery-nbody/)
[![Licence](https://img.shields.io/badge/licence-MIT-blue)](LICENCE)

A GPU-accelerated N-body gravitational simulator in C++20, developed and
benchmarked entirely on a single Lunar Lake laptop.

Orrery simulates the gravitational interaction of large numbers of point masses:
a direct O(N^2) solver and a Barnes-Hut O(N log N) solver, symplectic and
reference integrators, a threaded and vectorised CPU backend, a SYCL backend for
the integrated GPU, a real-time renderer and Python bindings. Two million
particles at 0.6 seconds per force evaluation, with no host-to-device copy
anywhere.

Nothing is claimed here that a command in this repository does not reproduce.

## The demonstration

Two disc galaxies of unequal mass on a bound, grazing encounter. They fall
together, pass, draw tidal tails out of one another, separate, return and merge.

There is no video file in this repository, and there is no need for one. These
are the commands that make it:

```
cmake --preset renderer
cmake --build --preset renderer
./build/renderer/apps/orrery run examples/collision.orrery \
    --set initial_conditions.count=20000 --set run.steps=6000 \
    --set output.trajectory_stride=20
./build/renderer/apps/orrery-view play collision.otj --export frames \
    --width 1920 --height 1080 --exposure 1.6
ffmpeg -framerate 60 -i frames/frame_%05d.ppm \
    -c:v libx264 -pix_fmt yuv420p -crf 18 collision.mp4
```

That run takes 122 seconds, 20.4 ms a step. It conserves energy to 3.3 parts in a
thousand and its virial ratio goes from 0.94 at the start to 0.99 at the end,
which is the merger virialising: two galaxies each in rough internal balance,
plus the orbital energy of the encounter between them, becoming one object in
balance with itself.

Drop the export and it is interactive:

```
orrery-view run examples/collision.orrery --set initial_conditions.count=20000
```

Drag to turn, scroll to zoom, `-` and `=` for the exposure, space to pause.
**The collision runs live at thirty thousand particles above thirty frames a
second, and a recorded run plays back at a million.** At that point the renderer
is drawing three thousand frames a second and waiting for the solver, which is
the right way round.

Every particle is drawn as a small round sprite with additive blending into a
floating-point target, because a picture of a galaxy is a sum of light along the
line of sight rather than a question of what is nearest, and a sum has no upper
bound. A tone mapping curve then compresses that range, which is what lets the
bright core and the faint outer arms appear in one image. There is no depth test
anywhere. The controls, the costs and what the galaxy model does and does not
claim to be are in [`docs/visualisation.md`](docs/visualisation.md) and ADR-0038.

## What has been demonstrated

The project's first goal is correctness that can be demonstrated, so the code is
compared against problems whose answers were known before it was written. The
full account, every analytic comparison and conservation result with the test
that makes it, is [the validation report](docs/validation.md). All of it runs
here:

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

That is 311 cases in about eleven seconds.

**The integrators converge at the orders they claim**, measured against a
circular orbit whose exact state after one period is the state it started in:

| Method | Force evaluations per step | Measured order | Relative energy error, first twentieth of the run | Last twentieth |
| --- | --- | --- | --- | --- |
| Velocity Verlet | 1 | 1.9998 | 2.6894e-3 | 2.6894e-3 |
| Yoshida 4 | 3 | 4.0006 | 2.0170e-5 | 2.0170e-5 |
| RK4 | 4 | 4.1670 | 1.92e-5 | 3.72e-4 |

The last column is the point, over four hundred orbits of an eccentric two-body
problem. The two symplectic methods end with the energy error they started with,
to six digits. RK4, of the same order as Yoshida and costing a third more per
step, is nineteen times further out by the end and still moving. ADR-0011 sets
out why that decides the default.

**The two-body problem closes.** The acceleration of a pair is exact, bit for
bit. An eccentric orbit released at periapsis returns to its starting state after
one period to 8.8e-6, and over two hundred revolutions keeps its semi-major axis
to 6.7e-4 and its eccentricity to 5.0e-4, neither drifting. The integrated period
agrees with `2 pi sqrt(a^3 / G M)` to better than a part in ten thousand.

The axis of that orbit turns by 9.7e-4 radians per revolution, and the test that
follows is what makes the number mean something. Only an exact inverse square law
gives a closed orbit, so a precession is either a wrong force law or an artefact
of the integrator. Halving the timestep divides it by 3.99, which is the
second-order behaviour of velocity Verlet and not a property of the physics.

**The initial conditions are what they claim to be.** A sampled Plummer sphere
comes back in virial equilibrium, with the median radius, the energies and the
profile of the model it was drawn from; a uniform sphere reproduces
`-3 G M^2 / 5 R`; every disc particle sits on the circular orbit its radius
supports.

**Conservation is measured rather than assumed.** Linear momentum holds to
round-off in the direct solver, 5e-17 of the terms that cancelled over 128
particles, and nothing in the kernel arranges for it: every pair is computed from
both ends (ADR-0015). Angular momentum holds to round-off under the symplectic
methods and only to its truncation error under RK4. The tree solver gives
momentum conservation up, and the suite measures the size of the violation
instead of asserting a zero that would be false.

**Approximation costs more accuracy than reduced precision does.** The tree
solver at its default opening angle has a root mean square error of 1.9e-3
against a compensated double-precision reference; the same physics in single
precision on the GPU has 3.7e-6, three orders of magnitude better. That one fact
decides where each is worth using.

**A run can be interrupted and resumed to bitwise-identical state**, for all
three integrators and both CPU solvers, through a real file on a disc. And a
threaded evaluation is bit for bit the unthreaded one at any thread count, which
is asserted for equality rather than against a tolerance.

## What has been measured

The second goal is performance that can be quantified, which here means that
every speed is a fraction of a ceiling measured on the same machine rather than a
timing in isolation. The full account is [the performance report](docs/performance.md).
Reproduce with:

```
./build/release/benchmarks/orrery_roofline 8192 21
./build/release/benchmarks/orrery_threading_scaling 8192 11
./build/release/benchmarks/orrery_tree_scaling
```

**One force evaluation, gathered into one place:**

| Solver and device | Particles | Per evaluation | Against a limit measured on the same machine |
| --- | --- | --- | --- |
| CPU direct, AVX2, 8 threads | 8192 | 13.80 ms | 79.7% of the divide and square root ceiling |
| CPU tree | 262144 | 868 ms | 20x direct summation, with cost going as N^1.24 |
| GPU direct | 65536 | 75.4 ms | 4.09x all eight CPU cores, at 1139 Gflop/s |
| GPU tree | 262144 | 59.8 ms | 12.2x the CPU tree |
| GPU tree, the largest run | 2097152 | 603 ms | 87.5 MiB shared, the host's Morton sort 40 to 46% of it |

Drawing is measured separately because it is a frame rate rather than an
evaluation: a recorded run plays back at a million particles and 126 frames a
second, and the live collision runs at thirty thousand above thirty.

Every row is a median with an interquartile range beside it in the report, not a
best of N, because the target is a laptop and a best-of on a part that throttles
reports the trial taken before the fan noticed (ADR-0019). The session behind the
first row spreads 1.3 to 5.0 per cent. The report also gives the session where
that row came out at 22.4 ms instead, with a 27 per cent spread and a thermal
canary at twice its starting duration, and says which of the two to believe and
why.

**This machine's real ceilings are not the ones on the specification sheet:**

| Probe | Sustained | Interquartile spread |
| --- | --- | --- |
| Read bandwidth | 95.7 GB/s | 1.5% |
| Triad bandwidth | 75.2 GB/s | 1.3% |
| Fused multiply-add | 330 Gflop/s | 2.2% |
| Divide and square root | 12.2 Gop/s | 3.3% |

The nominal memory bandwidth is about 135 GB/s, so a real program reaches 71 per
cent of it. The last row is the one that decides everything else: every pairwise
interaction contains one square root and one division, and those retire on a unit
with a twenty-seventh of the throughput of the multiply-add pipelines.

**Against those limits, the direct solver at 8192 particles:**

| Kernel | Threads | Per evaluation | flop/s | Of the peak | Of the divide ceiling |
| --- | --- | --- | --- | --- | --- |
| scalar | 1 | 177.6 ms | 7.56 G | 2.3% | 6.2% |
| scalar | 8 | 35.57 ms | 37.7 G | 11.4% | 30.9% |
| avx2 | 1 | 53.13 ms | 25.3 G | 7.6% | 20.7% |
| avx2 | 8 | **13.80 ms** | **97.3 G** | 29.5% | **79.7%** |

Vectorising is worth 3.34 times on one core and threading a further 3.85, for
12.87 times together, and 22.28 times in single precision. **The kernel is at
four fifths of the only ceiling it can compete for**, so what is left to win is
not in the code. Vectorising also made it *more* accurate, by a factor of 3.5,
because keeping one partial sum per lane turns one long sum into four short ones.
No fast-math flag is set anywhere in this project (ADR-0020).

**The scheduler tracks a hardware ratio nobody told it.** A performance core
computes the vector kernel 6.34 times as fast as an efficiency core, against 2.17
on the scalar kernel, so eight cores are worth 4.63 performance cores rather than
5.84:

| Scheme | Speedup over one performance core | Fraction of the 4.63 limit | Performance cores idle |
| --- | --- | --- | --- |
| Equal fixed shares | 3.63x | 78% | 83.4% |
| Work stealing | 4.13x | 89% | 8.2% |

The work-stealing scheduler was not changed between the two phases and holds no
weights, no calibration and no topology, yet the share it gave the performance
cores moved from 2.38 to one to 5.29 to one, tracking a ratio that had nearly
tripled. A weight tuned against the scalar kernel would have been wrong by a
factor of three, and wrong silently.

**The tree solver overtakes direct summation at about 6100 particles** and its
cost goes as N^1.24 over two and a half decades, against 1.11 for exactly
N log N. That crossover is high for a good reason: the opponent is an AVX2 kernel
at four fifths of its ceiling on eight cores. At 262144 particles the tree
computes 78 times fewer interactions and is only 20 times faster, because a tree
interaction costs about four times a direct one, and that gap rather than the
asymptotics is what a faster tree has to attack.

**The integrated GPU runs the same direct summation 4.1 times faster than all
eight CPU cores** at 65536 particles, reaching 1174 Gflop/s and 42 per cent of
the multiply-add ceiling measured on the device itself. It overtakes the CPU at
about 2200 particles and loses below that, since a kernel launch costs roughly
150 microseconds. **There is no host-to-device copy**, and that is demonstrated
by test rather than asserted.

**Walking the tree one sub-group at a time is about three times faster than
letting each work-item walk alone**, while stepping through 25 per cent *more*
nodes: the extra nodes are ones the hardware was already executing under a
divergence mask. It computes the same answer, and the suite requires the GPU and
CPU solvers' interaction counters to be *equal* rather than merely close
(ADR-0029).

So the fastest thing the project can do at a given size:

| Particles | Fastest | Because |
| --- | --- | --- |
| Below about 2200 | CPU direct, AVX2 on eight cores | A kernel launch costs more than the arithmetic saves |
| 2200 to about 9000 | GPU direct | Enough arithmetic to amortise the launch, not enough structure for a tree |
| Above about 9000 | GPU tree | The traversal is three times faster coherent |
| Above about 6100, without a GPU | CPU tree | The crossover against CPU direct summation |

**The largest tractable configuration is about 2.1 million particles**, at 603 ms
per evaluation and 87.5 MiB of shared memory, and nothing about the device
stopped it there. The limit is on the host: the Morton sort is 40 to 46 per cent
of every evaluation, which is the most useful thing the measurement says and the
first thing to fix next.

The GPU figures need the oneAPI compiler:

```
cmake --preset sycl-single-precision -DCMAKE_CXX_COMPILER=icx-cl
cmake --build --preset sycl-single-precision
./build/sycl-single-precision/benchmarks/orrery_sycl_direct
./build/sycl-single-precision/benchmarks/orrery_sycl_tree
```

## Running one

A run is a configuration file, and the file plus a revision of this repository
determines the trajectory:

```
cmake --preset release
cmake --build --preset release
./build/release/apps/orrery run examples/cluster.orrery
```

That is four thousand particles sampled from a Plummer sphere, integrated for a
thousand steps with the tree solver, writing a binary trajectory, a CSV
diagnostics stream and a checkpoint. Any setting can be overridden without
editing the file:

```
orrery run examples/cluster.orrery --set solver.kind=direct --set run.steps=100
orrery show examples/cluster.orrery          # every setting a run would use
orrery inspect cluster.otj                   # what is in an output file
```

The configuration format is small, strict and specified in
[`docs/formats/configuration.md`](docs/formats/configuration.md): an unknown
section, a mistyped setting or a value that does not parse is an error naming the
line it is on, because a run whose `softenning` was silently dropped is not a run
that failed but one that answered a question nobody asked (ADR-0031).

A checkpoint carries the configuration inside it, so resuming needs the file and
nothing else:

```
orrery resume cluster.ock
orrery resume cluster.ock --set run.steps=50000   # or go further than planned
```

The two binary formats are specified rather than dumped, in
[`docs/formats/trajectory.md`](docs/formats/trajectory.md) and
[`docs/formats/checkpoint.md`](docs/formats/checkpoint.md). A trajectory has no
frame count in its header and a checksum on each frame, so a file from a run that
was killed is a valid file that stops early rather than a broken one.

The second example is the two-body problem, and it is worth running for what the
diagnostics column shows:

```
orrery run examples/kepler.orrery
orrery run examples/kepler.orrery --set integrator.kind=rk4 \
    --set output.diagnostics_path=kepler-rk4.csv
```

Over three thousand orbits, velocity Verlet's relative energy error is 2.685e-3
in the first twentieth of the run and 2.689e-3 in the last, unchanged to four
digits. RK4 starts twenty times more accurate at 1.30e-4 and finishes at
2.783e-3, having just overtaken it and still growing. That is ADR-0011's
argument, visible in one column of a CSV file.

## Driving one from Python

The same simulator, from a notebook, with the particle state as NumPy arrays that
share memory with the run rather than copies of it:

```
pip install orrery-nbody
```

The distribution is `orrery-nbody` and the module it installs is `orrery`,
because `orrery` on PyPI belongs to an unrelated package that was there first.
Wheels are published for Linux, macOS and Windows, so nothing is compiled on the
way in. `pip install .` in a checkout builds the same package from source.

```python
import orrery

configuration = orrery.Configuration()
configuration.initial_conditions.kind = orrery.InitialConditionKind.plummer
configuration.initial_conditions.count = 4096
configuration.run.timestep = 1.0 / 64.0
configuration.run.steps = 1000
configuration.solver.kind = orrery.SolverKind.barnes_hut
configuration.solver.softening = 0.02

simulation = orrery.assemble(configuration)
before = simulation.measure()
simulation.run(configuration.run.steps)

x = simulation.particles.position_x   # a view of the solver's own memory
```

**Nothing is copied to read a state.** `x` is a NumPy array pointing into the
array the force kernel writes into, so watching a million-particle run costs
nothing per frame rather than eighty megabytes. That is asserted rather than
claimed: the test suite writes through a view and requires the C++ side to report
the change, then changes the state in C++ and requires the array to report that.

Positions are held as three contiguous arrays rather than as one array of
triples, because that is what makes the force kernel fast, so the interface
offers the components and names the copy when you want one (ADR-0040). A running
simulation's state is read-only, because the integrators require the
accelerations to belong to the current positions (ADR-0041). A `Configuration` is
the same record the configuration file parses into, so a run set up in Python is
the run `orrery run` would perform.

Three example notebooks are in [`python/notebooks/`](python/notebooks). Every
claim in them is asserted before it is plotted, so executing one is a check
rather than a rendering, and continuous integration executes all three. They
reproduce the project's validation results by a different route: the measured
convergence orders come out at 1.9998, 4.0006 and 4.1659 against stated orders of
2, 4 and 4. The interface, the lifetime rules and what is deliberately not bound
are in [`docs/python.md`](docs/python.md).

## Target hardware

Every performance decision in the project follows from one machine, so its
figures are recorded rather than left implicit:

| Property | Value |
| --- | --- |
| CPU | Intel Core Ultra 5 238V (Lunar Lake) |
| Cores | 4 performance plus 4 efficiency, 8 threads, no SMT |
| Vector width | AVX2, 256-bit |
| Memory | 32 GB LPDDR5X-8533, on package |
| GPU | Intel Arc 130V, Xe2, 7 Xe-cores, memory unified with the host |

Those are the manufacturer's figures. The measured ones, which are what the
project quotes, are in the section above and in the performance report.

## Building

Requirements: CMake 3.25 or later, a C++20 compiler, and Ninja for the presets
that use it. Catch2 is fetched at configure time, so the first configure of a
build tree needs network access. Nothing else has to be installed.

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

On Windows the same commands work, run from a Visual Studio developer prompt so
that MSVC and Ninja are on the path. There are no separate Windows presets: a
preset naming a Visual Studio generator has to name its version too, and that
version moves.

The presets are:

| Preset | What it is for |
| --- | --- |
| `debug` | Unoptimised with assertions. The default for working on the code |
| `release` | Optimised with debug information. Every performance figure comes from this |
| `sanitise` | Address and undefined-behaviour sanitisers, optimised so the suite stays quick enough to run |
| `thread-sanitise` | Thread sanitiser, for the scheduler. Separate because it cannot be combined with the address sanitiser |
| `single-precision` | Release with `float` rather than `double` as the scalar type |
| `sycl` | Release with the GPU backend. Needs the oneAPI DPC++ compiler |
| `sycl-single-precision` | The same with `float`. The configuration the GPU figures come from |
| `renderer` | Release with the viewer. Fetches GLFW and needs an OpenGL 3.3 driver |
| `python` | Release with the extension module, importable from the build tree |
| `lint` | Debug with clang-tidy running alongside the compiler. Needs Clang |

Every one of them is exercised by continuous integration, along with a
clang-format check, on Linux with GCC and Clang, on macOS with Clang, and on
Windows with MSVC.

The GPU backend is off by default and a build without it is complete rather than
degraded: the solvers are selected at run time and the GPU is one more of them.
Building it needs the oneAPI DPC++ compiler, which is none of the three the
project is otherwise tested with, so the compiler has to be named. On Windows
that is `icx-cl`, the MSVC-style driver, because CMake drives a Windows IntelLLVM
compiler with MSVC-style flags that `icpx` rejects; elsewhere it is `icpx`. Run
the oneAPI environment script first. On a machine with no device the suite still
passes: the GPU cases skip and the discovery layer is required to report no
device rather than fail.

The documentation site is one command and needs Doxygen 1.10 or later:

```
doxygen docs/Doxyfile
```

which writes `build/html/index.html`. Continuous integration runs the same
command on every pull request, with warnings as errors so that a dead link fails
a build, and publishes the result from `main` (ADR-0043).

## Repository layout

```
cmake/            Build settings, dependency pins, lint integration
include/          Public headers, under include/orrery/<layer>/
src/              Implementation, one directory per layer
apps/             The command-line simulator and the viewer
python/           The extension module, the package, its tests and its notebooks
examples/         Configuration files that run as they are
tests/            Catch2 test suite, one executable per layer
benchmarks/       Measurement programs. They report numbers rather than assert them
docs/             The two reports, the format specifications, the site configuration
docs/adr/         Numbered decision records, never edited after merge
docs/formats/     Specifications of the configuration, trajectory and checkpoint files
docs/performance/ Measured results, with the machine state that produced them
```

The layers are `apps/`, `sim/`, `solvers/`, `integrators/`, `backend/`,
`initial_conditions/` and `core/`, with dependencies pointing downwards only.
`viz/` sits beside `sim/` rather than under it: it depends on `core/` alone, it
does not know what a solver is and cannot read a file, and putting a simulation
and a picture of one together is the job of the layer above them both. `sim/`
owns a run, holding the solver, the integrator and the output, and it is the only
layer that knows files exist. `backend/` holds both execution backends: the CPU
thread pool and its schedulers, and the SYCL device discovery and unified memory
the GPU solver is built on. The GPU kernels themselves sit in `solvers/` beside
the CPU kernels they mirror, because a summation over pairs of particles is not a
scheduling policy (ADR-0026). `python/` is not a layer at all: it depends on
everything and nothing depends on it.

## Documentation

All of it is published at
[protonicwave.github.io/orrery](https://protonicwave.github.io/orrery/),
generated from the Markdown below and from the comments in the public headers by
one run of one tool, so a page there and the file it came from cannot drift
apart (ADR-0043).

- [Validation report](docs/validation.md): every analytic comparison,
  convergence study and conservation result, each naming the test that makes it.
- [Performance report](docs/performance.md): every speed, as a fraction of a
  limit measured on the same machine, and an account of which figures reproduce.
- [Visualisation](docs/visualisation.md): the viewer's controls, what it costs,
  the demonstration scenario and the path from a run to a video.
- [Python bindings](docs/python.md): installing, the zero-copy array interface,
  the lifetime rules, and the example notebooks.
- [File formats](docs/formats/): the configuration language, the binary
  trajectory and the checkpoint, each specified well enough to be read by
  something other than this program.
- [Architecture decision records](docs/adr/): why the design is the way it is,
  in forty-four short documents.
- [Contributing guide](CONTRIBUTING.md): conventions, testing categories and the
  definition of done.

## Licence

MIT. See [LICENCE](LICENCE).
