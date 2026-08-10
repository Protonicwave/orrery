# Orrery

A GPU-accelerated N-body gravitational simulator in C++20, developed and
benchmarked entirely on a single Lunar Lake laptop.

> **Status: under construction.** The repository holds the build system, the
> continuous integration pipeline, the conventions, the core data structures,
> the conserved quantities of a configuration, the configurations themselves, a
> Plummer sphere, an exact Kepler two-body orbit and a uniform sphere, the
> time integrators, velocity Verlet, Yoshida's fourth-order symplectic
> composition and classical RK4, the direct O(N^2) gravitational solver they
> advance a configuration under, a work-stealing scheduler that runs it across
> the machine's eight cores, an AVX2 kernel chosen at run time, and the
> benchmark harness that measures all of it against limits measured on the same
> machine. Orrery now simulates gravity correctly, and at four fifths of the
> ceiling that actually binds the kernel. What is missing is the algorithm: the
> solver still computes every pair, and the Barnes-Hut tree is the next phase.
> Progress is tracked in the phase table in
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

## What has been measured so far

Every performance figure this project quotes is a fraction of a limit measured
on this machine rather than a raw timing, and Phase 7 measured the limits. On a
quiet machine, in double precision:

| Probe | Sustained | Interquartile spread |
| --- | --- | --- |
| Read bandwidth | 95.7 GB/s | 1.5% |
| Triad bandwidth | 75.2 GB/s | 1.3% |
| Fused multiply-add | 330 Gflop/s | 2.2% |
| Divide and square root | 12.2 Gop/s | 3.3% |

The manufacturer's figure for memory bandwidth is about 135 GB/s, so a real
program reaches 71 per cent of it. The last row is the one that matters here.
Every pairwise interaction in the direct kernel contains one square root and one
division, and those retire on a unit with a twenty-seventh of the throughput of
the multiply-add pipelines, so no implementation of this algorithm on this part
can approach the peak.

Against those limits, the direct solver at 8192 particles:

| Kernel | Threads | Per evaluation | flop/s | Of the peak | Of the divide ceiling |
| --- | --- | --- | --- | --- | --- |
| scalar | 1 | 177.6 ms | 7.56 G | 2.3% | 6.2% |
| scalar | 8 | 35.57 ms | 37.7 G | 11.4% | 30.9% |
| avx2 | 1 | 53.13 ms | 25.3 G | 7.6% | 20.7% |
| avx2 | 8 | **13.80 ms** | **97.3 G** | 29.5% | **79.7%** |

Vectorising is worth 3.34 times on one core and threading a further 3.85, for
12.87 times together. The right-hand column is the result: the kernel is at four
fifths of the only ceiling it can compete for, so what is left to win is not in
the code. In single precision the same kernel reaches 192 Gflop/s, 22.3 times
the scalar single-threaded figure.

Vectorising also made the kernel **more** accurate, by a factor of 3.5 in double
precision and 5.7 in single, measured against a compensated-summation reference.
Keeping one partial sum per lane turns one sum of n terms into four or eight
sums of n/4 or n/8, and a shorter sum rounds less. No fast-math flag is set
anywhere in this project (ADR-0020).

The scheduler was re-measured on the vector kernel, and it settled a prediction
Phase 6 had written down. A performance core now computes this kernel **6.34**
times as fast as an efficiency core, against 2.17 on the scalar kernel, because
the two kinds of core differ far more in vector throughput than in scalar. Eight
such cores are worth 4.63 performance cores rather than 5.84.

| Scheme | Speedup over one performance core | Fraction of the 4.63 limit | Performance cores idle |
| --- | --- | --- | --- |
| Equal fixed shares | 3.63x | 78% | 83.4% |
| Work stealing | 4.13x | 89% | 8.2% |

The work-stealing scheduler was not changed between the two phases and holds no
weights, no calibration and no topology, yet the share it gave the performance
cores moved from 2.38 to one to 5.29 to one, tracking a hardware ratio that had
nearly tripled. A weight tuned in Phase 6 would have been wrong by a factor of
three by Phase 7, and wrong silently.

Threading and vectorisation change no result within a kernel. Each target reads
every source and writes only its own acceleration, so a threaded evaluation is
bit for bit identical to a serial one whatever the thread count, and the test
suite asserts that for equality rather than against a tolerance.

The full tables, the roofline plot, the per-worker breakdown and an honest
account of which of these figures reproduce and which do not are in
[`docs/performance/roofline.md`](docs/performance/roofline.md) and
[`docs/performance/threading.md`](docs/performance/threading.md). Reproduce
with:

```
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/orrery_roofline 8192 21
./build/release/benchmarks/orrery_threading_scaling 8192 11
```

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

The figures above are the manufacturer's. Phase 7 measured bandwidth and
throughput on this machine directly, and the measured values in the section
above are the ones the project quotes.

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
| `lint` | Debug with clang-tidy running alongside the compiler. Needs Clang |

Every one of them is exercised by continuous integration, along with a
clang-format check, on Linux with GCC and Clang, on macOS with Clang, and on
Windows with MSVC.

## Repository layout

```
cmake/            Build settings, dependency pins, lint integration
include/          Public headers, under include/orrery/<layer>/
src/              Implementation, one directory per layer
tests/            Catch2 test suite, one executable per layer
benchmarks/       Measurement programs. They report numbers rather than assert them
docs/             Implementation plan, architecture decision records
docs/adr/         Numbered decision records, never edited after merge
docs/performance/ Measured results, with the machine state that produced them
```

The source layers arrive with the phases that need them, in the structure
described in the implementation plan: `apps/`, `sim/`, `solvers/`,
`integrators/`, `backend/`, `initial_conditions/` and `core/`, with dependencies
pointing downwards only. `core/`, `backend/`, `initial_conditions/`,
`integrators/` and `solvers/` exist so far. `benchmarks/harness/` holds the
measurement infrastructure the benchmark programs share; it is not a layer of
the simulator and nothing under `src/` depends on it.

## Documentation

- [Implementation plan](docs/IMPLEMENTATION_PLAN.md): what is built, in what
  order, and what each phase has to demonstrate.
- [Contributing guide](CONTRIBUTING.md): conventions, testing categories and the
  definition of done.
- [Architecture decision records](docs/adr/): why the design is the way it is.

## Licence

MIT. See [LICENCE](LICENCE).
