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

## How it is built

- [The API reference](annotated.html), generated from the public headers.
- [Architecture decision records](adr/README.md). Forty-four short documents
  recording the decisions that had a credible alternative, each with its context
  and its consequences, none of them edited after it was merged.
- [The implementation plan](IMPLEMENTATION_PLAN.md). What was built, in what
  order, and what each phase had to demonstrate before it counted as done.
- [The measured results, phase by phase](performance/roofline.md), with the
  machine state that produced them and an account of which figures reproduce and
  which do not.
