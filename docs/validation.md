# The validation report

Every analytic comparison, convergence study and conservation result the project
makes, gathered into one document. Each claim names the test that makes it, so a
reader who does not believe one can run it rather than take it.

This is the report the project exists to be able to write. The project puts
correctness that can be demonstrated ahead of both speed and elegance, and what
follows is that priority spent: the code is compared
against problems whose answers were known before it was written, and never
against its own earlier output except where the claim being made is bitwise
reproducibility.

## Running all of it

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

That is 315 cases and about nine seconds on the machine described in
[the performance report](performance.md). Two cases skip where the device they
discover is not present, one for each GPU backend, and both report that they
skipped rather than passing quietly.

Each executable is a Catch2 binary, so a category or a subject can be run on its
own:

```
./build/release/tests/orrery_solvers_test "[validation]"
./build/release/tests/orrery_integrators_test "[property]"
./build/release/tests/orrery_core_test --list-tests
```

The suite runs in every preset in the table in the README. Four of them are
checks in their own right rather than convenience: `single-precision` runs the
same assertions against `float` with the tolerances the build's own epsilon
implies, `sanitise` and `thread-sanitise` run them under the address,
undefined-behaviour and thread sanitisers, and `sycl` adds the GPU cases.

## What is measured against what

Three references are used, and which one applies to a given claim is not a
matter of convenience:

**Analytic results.** Kepler's third law, the virial theorem for a Plummer
sphere, the potential energy of a uniform sphere, the acceleration of a two-body
pair at separations where the binary arithmetic is exact. These are the strongest
statements available, because nothing about them comes from this repository.

**The compensated reference kernel.** `src/solvers/reference_kernel.cpp` sums the
same softened force law with every term formed in `double` whatever the build's
scalar type, accumulated with Neumaier compensation, so its own summation error
is negligible against whatever is being measured. It is the instrument the
accuracy measurements use, and it is itself checked against the analytic
two-body case before it is trusted to judge anything: two masses of 2 and 3 at a
separation of 2 give accelerations of 3/4 and 1/2, every intermediate value is a
small exact binary fraction, and the test compares for equality rather than
against a tolerance.

**Direct summation in double precision.** The reference for every approximation:
the tree solver at each opening angle, the quadrupole option, the single
precision builds and both GPU solvers. It is not deleted once faster methods
exist, and it is what the faster methods have to answer to.

Comparing two approximations against each other would say they are wrong in the
same way rather than that either is right, and the project does that in exactly
one place, deliberately: the GPU solvers are compared against the reference
first and against the CPU solvers second, and the second comparison exists only
to catch a systematic offset that both could share.

## The two-body problem

The sharpest instrument in the project, because a two-body orbit has a closed
solution and an eccentric one is sensitive to the exact form of the force law
rather than only to its scale.

| What is checked | Result | Test |
| --- | --- | --- |
| Acceleration of a pair | Exact, bit for bit | `solvers/direct_solver_test.cpp` |
| Closure after one period | 8.8e-6 relative, at 20000 steps | `solvers/kepler_orbit_test.cpp` |
| Closure in single precision | 8.6e-4 at 2000 steps | the same |
| Semi-major axis over 200 orbits | 6.7e-4, not growing | the same |
| Eccentricity over 200 orbits | 5.0e-4, not growing | the same |
| Separation stays inside its annulus | Between periapsis and apoapsis throughout | the same |
| Integrated period against `2 pi sqrt(a^3 / G M)` | Better than a part in ten thousand | `integrators/convergence_test.cpp` |
| Energy and angular momentum from the elements | Agree with the generated state | `initial_conditions/kepler_test.cpp` |
| Period independent of eccentricity | Holds across the range sampled | the same |

The two closure figures are one result rather than two: ten times the step gives
a hundred times the error, which is what a second-order method promises and what
a mistake in the solver would not deliver.

**The precession test is the one worth reading twice.** The axis of the
eccentric orbit turns by 9.7e-4 radians per revolution, and a rotating axis is
either a force law that is not an exact inverse square, which Bertrand's theorem
makes the only other possibility for a closed orbit, or an artefact of the
integrator. The two are told apart by halving the timestep: an artefact falls by
four and a wrong exponent does not move. The measured ratio is 3.99. That is
velocity Verlet's second-order phase error and not the physics, and it is the
difference between bounding a number and explaining it.

## Convergence

Halving the timestep divides the error of a method of order p by 2^p, so the
ratio of the errors at two step sizes recovers p from a measurement. A method
whose arithmetic was subtly wrong would still integrate something and would still
produce plausible orbits; it would not produce the right power.

Measured on a circular orbit, whose exact state after one period is the state it
started in, so that no root-finder's error enters the quantity being measured:

| Method | Stated order | Measured | Force evaluations per step |
| --- | --- | --- | --- |
| Velocity Verlet | 2 | 1.9998 | 1 |
| Yoshida 4 | 4 | 4.0006 | 3 |
| RK4 | 4 | 4.1670 | 4 |

`integrators/convergence_test.cpp` requires each within 0.2 of its stated order
in double precision and within 0.6 in single, where the round-off floor sits
eight decades higher and the window between the asymptotic regime and the floor
is correspondingly narrower.

The same three orders come out of the Python notebooks by an independent route,
at 1.9998, 4.0006 and 4.1659, and continuous integration executes those notebooks
on every change.

## Energy behaviour, and the counterexample

The most informative single comparison the project makes, and the reason it
integrates with a second-order method by default rather than a fourth-order one
(ADR-0011).

An eccentric orbit at 200 steps per orbit, integrated for 400 orbits, energy
sampled twenty times per orbit so that the envelope of the oscillation is
measured rather than one point on it:

| Method | Relative energy error, first twentieth | Last twentieth | Behaviour |
| --- | --- | --- | --- |
| Velocity Verlet | 2.6894e-3 | 2.6894e-3 | Bounded |
| Yoshida 4 | 2.0170e-5 | 2.0170e-5 | Bounded |
| RK4 | 1.92e-5 | 3.72e-4 | Growing |

The two symplectic methods return the same error at the end of four hundred
orbits as they had at the start, to six digits. A symplectic integrator does not
conserve the energy of the system it was given; it conserves the energy of a
nearby system exactly, and the difference depends on the timestep rather than on
how long the run has been going. RK4 has no such structure to fall back on. Its
error is an accumulation, it is nineteen times further out by the end of this run
and still moving, and it costs four force evaluations a step against Yoshida's
three to get there.

`integrators/energy_behaviour_test.cpp` states that as three assertions: both
symplectic methods stay inside an envelope, the late window is no worse than
twice the early one, and RK4's late window is at least ten times its early one.
The bounds are loose on purpose. The claim is about the behaviour of the methods
and not about the last digits of one machine's arithmetic.

The same result appears in a column of a CSV file from the command line, over
three thousand orbits rather than four hundred:

```
orrery run examples/kepler.orrery
orrery run examples/kepler.orrery --set integrator.kind=rk4 \
    --set output.diagnostics_path=kepler-rk4.csv
```

Velocity Verlet gives 2.685e-3 in the first twentieth and 2.689e-3 in the last.
RK4 starts twenty times more accurate at 1.30e-4 and finishes at 2.783e-3, having
just overtaken it.

## Conservation

| Quantity | Holds to | Where |
| --- | --- | --- |
| Linear momentum, direct solver | 5e-17 of the terms that cancelled, over 128 particles | `solvers/conservation_test.cpp` |
| Linear momentum, all three integrators | Round-off, 200 steps of a 64-body cluster | `integrators/conservation_test.cpp` |
| Linear momentum, threaded solver | Unchanged by the thread count | `solvers/parallel_direct_solver_test.cpp` |
| Angular momentum, symplectic methods | Round-off | `integrators/conservation_test.cpp` |
| Angular momentum, RK4 | A thousandth of its own size, and no better | the same |
| Mass-weighted acceleration sum, GPU direct | 6.2e-9 of the scale of its terms | `solvers/sycl_direct_solver_test.cpp` |

Two of those rows are statements about the difference between the two families of
method rather than about their accuracy. Linear momentum is a linear function of
the state and every stage of every method here changes it by a sum that cancels
term by term, so all three keep it. Angular momentum is quadratic, and only the
methods with the symplectic structure keep it exactly; RK4 keeps it as a
consequence of being accurate, which means it stops keeping it as the timestep
grows. That is asserted as a weak bound rather than a tight one, because the
point is the mechanism and not the number.

Momentum conservation in the direct solver is worth stating plainly because
nothing arranges for it. The kernel computes every pair from both ends rather
than applying Newton's third law to halve the work (ADR-0015), so the
cancellation to 5e-17 is arithmetic falling out rather than bookkeeping being
enforced, and it would break immediately if a target were reading the wrong
source range.

**The tree solver gives this one up, and the suite says so rather than
pretending otherwise.** A tree is not symmetric: particle i may see j through a
cell while j sees i directly. `solvers/barnes_hut_test.cpp` measures the size of
the violation and requires that closing the opening angle reduces it, which is
the true statement, instead of asserting a zero that is not.

## The equilibrium models

An initial condition that is not what it claims to be invalidates everything
integrated from it, so the samplers are validated against the models they sample
before any run uses them.

**The Plummer sphere.** In the standard N-body units the model's analytic
energies are exactly -1/4 total, -1/2 potential and 1/4 kinetic, and that is
checked first as a statement about the model. The sample is then checked against
it:

| What is checked | Bound | Why the bound is what it is |
| --- | --- | --- |
| Virial ratio near unity | 0.10 | The largest departure over two dozen seeds was 0.04. Sampling speeds uniformly lands the ratio near 2; omitting the escape-speed scaling lands it near 0 |
| Potential and kinetic energies | 0.08 of the value | The same sampling scatter, measured at 0.03 |
| Median radius against 1.3048 scale radii | 0.08 of the value | The median of the model's cumulative mass profile, inverted at a half |
| Outermost particle | 40 scale radii | The mass-fraction cutoff at 0.999, whose radius is 38.7 |
| Centre of mass and total momentum | Round-off | A property of the sampler rather than of the model, which is centred by definition |

The virial ratio is the load-bearing one. A Plummer sphere is a self-consistent
equilibrium, so a correct sample has twice its kinetic energy equal to the
magnitude of its potential energy, and almost nothing the sampler could get
wrong in either the radial profile or the velocity distribution leaves that
intact.

**The uniform sphere.** Potential energy of `-3 G M^2 / 5 R`, within five per
cent of the sampled value, and an eighth of the particles inside half the radius
against a binomial standard deviation of 21. A radius drawn uniformly rather
than as a cube root would put a quarter of them there and would deepen the energy
by tens of per cent.

**The disc galaxy and the collision.** Every disc particle sits on the circular
orbit its radius supports in the enclosed mass of the model, including the
correction the softening makes to the circular speed; the angular momentum lies
along the spin axis; and an encounter given an approach speed of one is
parabolic, which is checked against its own energy. The galaxy is a scenario
rather than an equilibrium model and ADR-0038 says so; what is validated is that
it is the scenario it claims to be.

## The diagnostics themselves

Every conservation result above is a measurement made by the diagnostics, so the
diagnostics are validated separately rather than trusted.

The most important of those tests is `core/softening_test.cpp`'s finite
difference. Energy conservation compares a potential energy against the work done
by an acceleration, and the comparison means nothing unless the two come from the
same potential. The test differences the potential over 500 randomly drawn
separations either side of the softening length, with the step chosen as the cube
root of the machine epsilon so that truncation and rounding balance, and requires
the result to be the acceleration the force kernel uses. That is ADR-0008 turned
into a measurement: one softening definition, shared, or else the conservation
tests measure an artefact of the diagnostic rather than the physics.

The rest of the diagnostics are pinned by their symmetries. The potential energy
does not depend on where the configuration sits, it scales as the reciprocal of
the size, each quantity of a two-body configuration equals its hand-computed
value, and the momentum sum keeps digits an ordinary running total loses.

## How accurate the kernels are

Against the compensated reference, over a 2048-particle Plummer sphere with a
fixed seed:

| Build | Kernel | Worst relative error | Root mean square |
| --- | --- | --- | --- |
| double | scalar | 3.24e-15 | 8.8e-16 |
| double | avx2 | 9.5e-16 | 2.5e-16 |
| float | scalar | 1.88e-6 | 4.78e-7 |
| float | avx2 | 3.60e-7 | 8.35e-8 |

`solvers/kernel_accuracy_test.cpp` requires every available kernel to stay inside
`200 sqrt(N) epsilon`, which is 2.1e-12 in double precision and 1.1e-3 in single,
and separately requires the vector kernel to be no worse than the scalar one.

**Vectorising the sum improves it**, by a factor of 3.5 in double precision and
5.7 in single. Keeping one partial sum per lane turns one sum of n terms into
four or eight sums of n/4 or n/8, and a shorter sum rounds less. The
reassociation a vector kernel is usually apologised for is where some of the
error goes rather than where it comes from. No fast-math flag is set anywhere in
this project (ADR-0020), and the one compiler that defaults to relaxed
arithmetic is explicitly put back to strict, because a compiler permitted to
reassociate may delete the compensation in the reference kernel and leave the
project measuring everything against nothing.

## How accurate the approximations are

The tree solver's error is the opening angle's, and it is characterised as a
curve rather than quoted as a number. At 16384 particles, against the compensated
reference:

| Opening angle | Quadrupole | Worst error | Root mean square | Cost |
| --- | --- | --- | --- | --- |
| 0.2 | no | 3.53e-04 | 1.11e-04 | 123.3 ms |
| 0.4 | no | 4.57e-03 | 1.06e-03 | 50.08 ms |
| 0.5 | no | 1.04e-02 | 1.94e-03 | 34.55 ms |
| 0.7 | no | 2.52e-02 | 4.36e-03 | 19.33 ms |
| 1.0 | no | 6.65e-02 | 1.01e-02 | 11.33 ms |
| 0.5 | yes | 7.68e-03 | 9.76e-04 | 43.87 ms |

Fitted across the range the root mean square error goes as `theta^2.8` without
quadrupole moments and `theta^3.4` with them. The full table and the
error-against-cost analysis that decides the default are in
[`performance/barnes_hut.md`](performance/barnes_hut.md).

Three assertions hold the shape of that curve in the suite. The error falls
monotonically in both statistics as the angle closes, which is what says the
angle controls the error rather than merely correlating with it. The quadrupole
term improves the accuracy at a fixed set of accepted cells, which a sign error
in the contraction would not. And **an opening angle of zero reproduces direct
summation**, to 2e-13, having accepted no cells at all and computed exactly
`N (N - 1)` pairs: the same physics reached by a different route, through a
different particle order and a different loop.

In single precision the direct kernel's own error is 3.7e-6 at 65536 particles
against the same reference, three orders of magnitude better than the tree's
approximation at its default angle. **Approximating the physics costs far more
accuracy than computing it in single precision does**, and that is the single
most useful accuracy fact the project has measured.

## The GPU backends

A second implementation of physics that already has one is where projects of this
kind decay, so the GPU solvers are held to agreement rather than to plausibility.

| What is checked | Requirement | Test |
| --- | --- | --- |
| GPU direct against the reference | Worst 2.0e-4, RMS 2.0e-5 in single precision | `solvers/sycl_direct_solver_test.cpp` |
| GPU direct against the CPU solver | Within twice the reference bound | the same |
| GPU tree against the CPU tree | Interaction counters **equal**, accelerations to rounding | `solvers/sycl_tree_solver_test.cpp` |
| Both device traversals against each other | The same sum | the same |
| Every sub-group width the device offers | The same sum | the same |
| Opening angle of zero on the GPU | Reduces to direct summation | the same |
| A particle at the origin | Survives the padded launch | both |
| No host-to-device copy | Demonstrated, not asserted | `backend/sycl_usm_test.cpp` |

The interaction-counter equality is the strongest of those and it is a deliberate
consequence of ADR-0029. The published warp-coherent traversals let a lane that
would have accepted a cell descend with the rest of its warp, which makes a
particle's acceleration depend on which other particles shared its warp. This one
masks instead, so the suite can require that the device opened the same cells and
summed the same pairs, rather than that it landed somewhere nearby.

The zero-copy claim is demonstrated in three ways rather than argued. A test
writes an allocation from the host, has a kernel read and modify it through the
same pointer value, and reads it back without calling any copy, map or transfer
operation; it then asks the runtime what kind of pointer it holds and requires the
answer to be shared, and requires an ordinary heap pointer to answer otherwise so
that the query is discriminating rather than agreeable. The solver is then asked
the same question about the arrays its force kernel actually read, after a real
evaluation.

Where there is no Intel GPU the device cases skip and say so, and the discovery
layer is required to report no device rather than to throw. Continuous
integration runs that path on every change, since no hosted runner has an Arc
part.

## Determinism

Randomness is seeded explicitly, the seed is printed in the failure message, and
the same seed samples the same configuration. That is a precondition for
everything above: a property test that cannot be re-run on the configuration that
failed it is not evidence of anything.

Four stronger statements are asserted for equality rather than against a
tolerance:

- **A threaded evaluation is bit for bit the unthreaded one**, at any thread
  count. Each target reads every source and writes only its own acceleration, so
  there is nothing for the scheduler to reorder.
- **The tree does not depend on the threads that built it**, and neither does the
  Morton ordering that feeds it.
- **A kernel gives the same answer every time it is asked**, and a solver reused
  across evaluations gives the same answer as a fresh one.
- **An interrupted run resumes to bitwise-identical state**, for all three
  integrators and both CPU solvers, through a real file on a disc. Every bit of
  every position, velocity, acceleration and mass.

The last of those decided several things in the driver. The clock is the step
counter times the timestep rather than an accumulated sum, because a million
additions differ from one multiplication in their last bits; the checkpoint
stores the accelerations rather than recomputing them (ADR-0032); and it is
written to a temporary and renamed over the target, because the moment a run is
killed is not chosen to avoid the moment its checkpoint is half written.

## The suite as a whole

The release preset registers 315 cases. By the four categories the contributing
guide defines:

| Category | Cases | What it means here |
| --- | --- | --- |
| Validation | 39 | Comparison against a known analytic result or the compensated reference |
| Property | 37 | An invariant over randomised inputs with a fixed seed |
| Regression | 3 | A golden output, asserted for equality |
| Unit | 165 | A component in isolation |

The remaining 71 are the driver and file-format cases, tagged by subject rather
than by category, and they are where the configuration language, the trajectory
format, the checkpoint and the run loop are held to their specifications.

A `sycl` build adds 19 more, the device cases, which are compiled out rather
than skipped where the backend is off. A `cuda` build adds its own the same way,
and no count is given for them because no machine this project is developed on
can produce one. What is in the count above is the part of both backends that
needs no device: the version formatting, the allocation bookkeeping, and the two
discovery cases that ask for a device and report skipping when there is none. The renderer adds none, and that is
deliberate: the 17 `viz` cases are in the count above and every one of them runs
in a build with no renderer on a machine with no display, because the camera and
the tone mapping curve are arithmetic and a mistake in them appears on screen as
a black window rather than as a crash.

## What is not validated

Stating this is part of the report rather than an appendix to it.

**There is no analytic solution to compare a many-body run against.** Beyond two
bodies the problem is chaotic, so a cluster or a collision is checked through its
invariants, its diagnostics and the reversibility of its integrator, and not
against a known trajectory. Nothing in this project claims a particular
long-duration N-body trajectory is correct in detail, and nothing could.

**The tree solver's accuracy is not characterised in single precision.** It
builds and its tests pass there, but the sweep would be measuring the float
kernel's own error against the approximation at the tighter angles, and
separating the two is a measurement no phase has made.

**The tree does not conserve momentum**, as above. This is a property of the
method and not a defect, and the suite measures it rather than asserting a zero.

**The renderer's drawing is not tested.** No case opens a window. What is tested
is everything under it that is arithmetic: the camera, the projection, the tone
mapping curve, the sRGB encoding and the frame writer. The drawing itself is
checked by looking at it.

**The sanitiser coverage has two holes**, both recorded where they were found.
The address sanitiser cannot be combined with `-fsycl` at all, because every
translation unit is compiled for the device as well as the host and the device
target rejects the flag, so the SYCL kernels are the one part of the project it
never covers. And the undefined-behaviour sanitiser does not build on the
development machine, for a C runtime mismatch inside its own runtime library, so
it is continuous integration's to run. Both are set out in
[`performance/barnes_hut.md`](performance/barnes_hut.md) and
[`performance/sycl_direct.md`](performance/sycl_direct.md).

**The GPU evidence comes from one device.** Every figure and every device
assertion was taken on one Intel Arc 130V. The suite is written so that another
device would have to agree with the CPU rather than with a recorded number, but
no other device has run it.
