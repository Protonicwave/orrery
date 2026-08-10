# Orrery

A GPU-accelerated N-body gravitational simulator in C++20, developed and
benchmarked entirely on a single Lunar Lake laptop.

> **Status: under construction.** The repository holds the build system, the
> continuous integration pipeline, the conventions, the core data structures,
> the conserved quantities of a configuration, the configurations themselves, a
> Plummer sphere, an exact Kepler two-body orbit and a uniform sphere, the
> time integrators, velocity Verlet, Yoshida's fourth-order symplectic
> composition and classical RK4, and the direct O(N^2) gravitational solver they
> advance a configuration under. Orrery now simulates gravity, correctly and
> slowly: the solver is single-threaded and scalar, and making it fast is the
> subject of the next two phases. Progress is
> tracked in the phase table in
> [`docs/IMPLEMENTATION_PLAN.md`](docs/IMPLEMENTATION_PLAN.md), and this README
> gains results and figures as the phases that produce them land. Nothing is
> claimed here before it can be reproduced.

## What this is

Orrery simulates the gravitational interaction of large numbers of point masses.
It aims to be fast enough and accurate enough to be worth taking seriously on
consumer laptop hardware, and it is built around three goals in this order:

1. **Correctness that can be demonstrated.** The code is validated against
   problems with known analytic solutions, Kepler orbits and the virial ratio of
   a Plummer sphere among them, rather than against its own earlier output.
2. **Performance that can be quantified.** Speed is reported as a fraction of
   measured hardware limits, using a roofline model built from bandwidth and
   throughput figures measured on the target machine, not as raw timings in
   isolation.
3. **Engineering that survives inspection.** Any single file should be
   defensible on its own terms to a reviewer who did not write it.

The planned capability is a direct O(N^2) solver and a Barnes-Hut O(N log N)
solver, symplectic and reference integrators, a threaded and vectorised CPU
backend, a SYCL backend for the integrated GPU, a real-time renderer, and Python
bindings. The order in which those arrive, and what each has to demonstrate
before it is considered done, is set out in the implementation plan.

## What this is not

Deliberately out of scope, each a reasonable extension and none of them planned:

- General relativity.
- Hydrodynamics.
- Collisional stellar dynamics with regularisation.
- Distributed execution across multiple nodes.

It is also not a general physics framework. Orrery is a gravitational N-body
simulator with a layered architecture, and it stays that way until there is a
genuine second solver to justify anything more.

## What has been demonstrated so far

Every figure below is produced by the test suite, on the machine described in the
next section, and reproduced by:

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The integrators are measured against a circular two-body orbit, whose exact
solution after one period is the state it started in, and against an eccentric
one integrated for four hundred orbits at two hundred steps each:

| Method | Force evaluations per step | Measured order | Relative energy error, first twentieth of the run | Last twentieth |
| --- | --- | --- | --- | --- |
| Velocity Verlet | 1 | 1.9998 | 2.6894e-3 | 2.6894e-3 |
| Yoshida 4 | 3 | 4.0006 | 2.0170e-5 | 2.0170e-5 |
| RK4 | 4 | 4.1670 | 1.92e-5 | 3.72e-4 |

The last column is the point. The two symplectic methods return the same energy
error at the end of four hundred orbits as they had at the start, to six digits.
RK4, of the same order as Yoshida and costing a third more per step, is nineteen
times further out by the end and still moving. ADR-0011 sets out why that decides
the default.

The integrated orbital period agrees with `2 pi sqrt(a^3 / G M)` to better than a
part in ten thousand, linear momentum is conserved to round-off by all three
methods, and angular momentum to round-off by the symplectic pair.

The direct solver is measured against the same kind of standard. The acceleration
of a two-body pair is exact, bit for bit, at separations where the arithmetic is
exact. Total linear momentum is conserved to 5e-17 of the terms that cancelled to
produce it, over a hundred and twenty-eight particles: that is round-off and
nothing else, and it is worth stating because the kernel computes each pair from
both ends and nothing in it arranges for the cancellation (ADR-0015). An eccentric
orbit released at periapsis returns to its starting state after one period to
8.8e-6, and over two hundred revolutions keeps its semi-major axis to 6.7e-4 and
its eccentricity to 5.0e-4, neither of them drifting.

The axis of that orbit turns by 9.7e-4 radians per revolution, and the last test
in the file is what makes the number mean something. Only an exact inverse square
law gives a closed orbit, so a precession is either a wrong force law or an
artefact of the integrator. Halving the timestep divides it by 3.99, which is the
second-order behaviour of velocity Verlet and not a property of the physics.

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

The figures above are the manufacturer's. The project measures bandwidth and
throughput on this machine directly, and once it does, the measured values are
the ones it quotes.

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
| `single-precision` | Release with `float` rather than `double` as the scalar type |
| `lint` | Debug with clang-tidy running alongside the compiler. Needs Clang |

Every one of them is exercised by continuous integration, along with a
clang-format check, on Linux with GCC and Clang, on macOS with Clang, and on
Windows with MSVC.

## Repository layout

```
cmake/         Build settings, dependency pins, lint integration
include/       Public headers, under include/orrery/<layer>/
src/           Implementation, one directory per layer
tests/         Catch2 test suite, one executable per layer
docs/          Implementation plan, architecture decision records
docs/adr/      Numbered decision records, never edited after merge
```

The source layers arrive with the phases that need them, in the structure
described in the implementation plan: `apps/`, `sim/`, `solvers/`,
`integrators/`, `backend/`, `initial_conditions/` and `core/`, with dependencies
pointing downwards only. `core/`, `initial_conditions/`, `integrators/` and
`solvers/` exist so far.

## Documentation

- [Implementation plan](docs/IMPLEMENTATION_PLAN.md): what is built, in what
  order, and what each phase has to demonstrate.
- [Contributing guide](CONTRIBUTING.md): conventions, testing categories and the
  definition of done.
- [Architecture decision records](docs/adr/): why the design is the way it is.

## Licence

MIT. See [LICENCE](LICENCE).
