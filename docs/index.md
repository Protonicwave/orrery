# Orrery

A GPU-accelerated N-body gravitational simulator in C++20, developed and
benchmarked entirely on a single Lunar Lake laptop.

This is the documentation site. It is generated from the Markdown in the
repository and from the comments in the public headers, by one run of one tool,
so a page here and the file it came from cannot drift apart. The source is at
[github.com/Protonicwave/orrery](https://github.com/Protonicwave/orrery).

Orrery simulates the gravitational interaction of large numbers of point masses.
It aims to be fast enough and accurate enough to be worth taking seriously on
consumer laptop hardware, and it is built around three goals in this order:
correctness that can be demonstrated, performance that can be quantified, and
engineering that survives inspection.

It is a gravitational N-body simulator and not a framework for physics in
general. General relativity, hydrodynamics, collisional stellar dynamics with
regularisation and distributed multi-node execution are all outside it. Each is
a reasonable extension and none is in scope, because a framework with one solver
in it is a solver with extra indirection.

## The two reports

Everything the project claims is in one of these, and every claim in them names
the test or the command that produces it.

- [The validation report](validation.md). Every analytic comparison, convergence
  study and conservation result: Kepler orbits closing, the measured orders of
  three integrators, a symplectic method holding its energy error for four
  hundred orbits while a fourth-order method of the same cost drifts away from
  it, a sampled Plummer sphere in virial equilibrium, and what happens to
  momentum when a tree replaces the pairs.
- [The performance report](performance.md). Every speed, each as a fraction of a
  limit measured on the same machine: the AVX2 kernel at four fifths of the only
  ceiling that binds it, a work-stealing scheduler that tracks a hardware ratio
  nobody told it about, N^1.24 scaling over two and a half decades, a GPU tree
  traversal three times faster for walking one sub-group at a time, and two
  million particles at 0.6 seconds a force evaluation.

## Using it

- [Python bindings](python.md). Installing the package, the NumPy arrays that
  share memory with a running solver rather than copying it, the lifetime rules,
  and the example notebooks.
- [Visualisation](visualisation.md). The viewer's controls, what drawing costs,
  the galaxy collision scenario and the documented path from a run to an encoded
  video.
- [File formats](formats/configuration.md). The configuration language, and the
  [trajectory](formats/trajectory.md) and [checkpoint](formats/checkpoint.md)
  binaries, each specified well enough to be read by something other than this
  program.
- [The instrument](instrument.md). The browser client, which plays a trajectory
  this repository produced and draws it with the same optics as the native
  renderer. What it draws, how a published run is made, what the drawing costs,
  and where to find it: it is
  [live here](https://protonicwave.github.io/orrery/instrument/). Every value in
  it comes from a file in the repository rather than from a copy of one
  (ADR-0045). Beside it is
  [the reading half](https://protonicwave.github.io/orrery/instrument/method/):
  the same argument as the two reports above, at the length a page should be,
  with every figure traced back to the report it came from.
- [The solver in a browser](webassembly.md). The same C++ compiled to
  WebAssembly and stepped in a Worker, so the instrument can compute a run as
  well as play one. What is in that build, what it refuses, what a step costs
  there against what it costs natively, and the test that says the two builds
  agree (ADR-0051, ADR-0052).

## What the design rests on

Five decisions shape the rest of the code, and each of them is load-bearing
enough that changing it would change everything above it.

**Particles are stored as separate contiguous arrays rather than as an array of
structs.** The force kernel reads positions and masses and nothing else. Under an
array-of-structs layout every cache line it fetched would also carry velocities
and accelerations it never touches, wasting a large share of the bandwidth that
already binds. Separate arrays also give contiguous vector loads instead of
strided gathers.

**Virtual dispatch sits at boundaries and never inside a loop.** Solvers and
backends are selected at run time, so a benchmark or a test can swap
implementations from a flag. The cost is one indirect call per timestep ahead of
billions of floating-point operations, and it is unmeasurable. No virtual call
appears in any loop over particles.

**A GPU implementation is a backend behind the solver interface, not a second
copy of the solver.** Two divergent implementations of the same physics is the
usual way a project of this kind decays, and ADR-0026 puts the device behind the
interface rather than in front of it.

**Precision is selected at build time.** `Real` is `double` by default and
`float` under a build option. Templating every solver on the scalar type would
multiply compile times and complicate the SYCL kernels for no practical gain,
because a given run is either accuracy-oriented or throughput-oriented and never
both.

**The direct solver is the reference and is never deleted.** Every
approximation, whether an opening angle, a multipole order or a reduced
precision, is measured against direct summation in double precision.

## How it is built

- [The API reference](annotated.html), generated from the public headers.
- [Architecture decision records](adr/README.md). Fifty-two short documents
  recording the decisions that had a credible alternative, each with its context
  and its consequences, none of them edited after it was merged.
- [The measured results, subsystem by subsystem](performance/roofline.md), with
  the machine state that produced them and an account of which figures reproduce
  and which do not.
