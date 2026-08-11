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
> the machine's eight cores, an AVX2 kernel chosen at run time, the benchmark
> harness that measures all of it against limits measured on the same machine,
> the Barnes-Hut tree solver that replaces most of the interactions with an
> approximation whose error is measured against the direct one rather than
> assumed, a SYCL backend that runs the direct kernel on the integrated Arc
> GPU, and the tree traversal on that GPU as well, walked one sub-group at a
> time so that the lanes sharing an instruction pointer stop waiting for each
> other. Orrery now simulates gravity correctly, at four fifths of the ceiling
> that binds the direct kernel on the CPU, at a cost that grows as N log N
> rather than N^2, with no host-to-device copy anywhere, and it does so for two
> million particles at about 0.6 seconds per force evaluation. What is missing
> is the simulation driver, the renderer and the bindings. Progress is tracked
> in the phase table in
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

## What the tree solver changed

The direct solver computes every pair and is the reference everything else is
measured against. The Barnes-Hut solver replaces distant groups of particles
with a single term each, which turns the cost from N^2 into something close to
N log N and introduces an error the opening angle controls.

Measured over two and a half decades of N, against the same vectorised
eight-core direct solver above:

| N | Tree | Direct | Speedup | Interactions per particle |
| --- | --- | --- | --- | --- |
| 1024 | 0.879 ms | 0.283 ms | 0.32x | 647 |
| 4096 | 5.573 ms | 4.417 ms | 0.79x | 1403 |
| 8192 | 15.47 ms | 18.40 ms | **1.19x** | 1852 |
| 32768 | 96.00 ms | 400.3 ms | 4.17x | 2580 |
| 65536 | 236.3 ms | 1082 ms | 4.58x | 2953 |
| 262144 | 868.2 ms | about 17 s | about 20x | 3373 |

The crossover is at about **6100 particles**, which is high compared with the
figures usually quoted for Barnes-Hut and is high for a good reason: the
opponent is an AVX2 kernel running at four fifths of its hardware ceiling on
eight cores, not a scalar loop. Direct summation at 262144 particles is not
measured but extrapolated from its own N^2 scaling, and is marked as such.

Fitted across the whole range the cost goes as **N^1.24**, against 1.11 for
exactly N log N and 2.0 for direct summation. The tree depth grows from 6 to 11
levels over the same range, which is log N almost exactly.

The interaction counter says something the timings cannot. At 262144 particles
the tree computes **78 times fewer** interactions than direct summation and is
only 20 times faster, because a tree interaction costs about four times as much
as a direct one: the direct kernel computes four pairs per AVX2 register from
contiguous memory, and the tree's cell terms are scalar and sit at the end of a
walk. That gap, rather than the asymptotics, is what a faster tree has to
attack next.

Accuracy is measured against a compensated-summation reference, not against the
direct solver, since both of those have rounding error of their own. At 16384
particles, with the default opening angle of 0.5, the root mean square relative
error is 1.9e-3 and the worst particle is at 1.0e-2; closing the angle to 0.2
brings those to 1.1e-4 and 3.5e-4 for 3.6 times the time. Quadrupole moments are
an option rather than a default, because the measurement says they are the
cheaper way to buy accuracy only below about a part in a thousand:

| Target RMS error | Monopole only | With quadrupoles | Cheaper |
| --- | --- | --- | --- |
| 1e-2 | 11.3 ms | not reachable at any legal angle | monopole |
| 1e-3 | 43.5 ms | 43.9 ms | neither |
| 1e-4 | 127 ms | 90 ms | quadrupole, by 1.4x |

Momentum is the one invariant the tree gives up. Direct summation conserves it
to round-off because it evaluates each pair from both ends; a tree does not,
since particle i may see j through a cell while j sees i directly. The test
suite measures the size of that violation and that closing the angle reduces it,
rather than asserting a zero that would be false.

## What the GPU changed

The integrated Arc 130V runs the same direct summation as a backend behind the
same solver interface, in SYCL, sharing physical memory with the CPU. Measured
in single precision against all eight CPU cores running the AVX2 kernel:

| N | GPU | CPU | Speedup | Staging | Gflop/s |
| --- | --- | --- | --- | --- | --- |
| 1024 | 0.178 ms | 0.119 ms | 0.67x | 1.0% | 117 |
| 2048 | 0.351 ms | 0.334 ms | 0.95x | 0.5% | 253 |
| 8192 | 1.555 ms | 4.511 ms | 2.90x | 0.3% | 890 |
| 65536 | 75.39 ms | 308.5 ms | **4.09x** | 0.2% | 1139 |
| 131072 | 297.1 ms | not timed | | 0.1% | **1174** |

The GPU overtakes the CPU at about **2200 particles** and loses below that: a
kernel launch costs roughly 150 microseconds, and a thousand-body simulation
should stay on the CPU.

**There is no host-to-device copy**, which is demonstrated rather than asserted.
A test writes an allocation from the host, has a kernel read and modify it
through the same pointer value, reads it back, and never calls any copy or map
operation; it then asks the runtime what kind of pointer it is holding and
requires the answer to be shared, and requires an ordinary heap pointer to
answer otherwise so the query is discriminating. The staging the solver does
perform is host memory to host memory, costs 0.1 to 1.0 per cent of an
evaluation, and exists only because this driver does not report
`usm_system_allocations` (ADR-0027).

Against ceilings measured on the device itself, the kernel reaches 42 per cent
of its multiply-add limit. Unlike the CPU kernel it is **not** bound by the
square root and division in every interaction: this GPU's divide unit is only
4.1 times slower than its multiply-add pipelines, against 27 times on the CPU,
so the kernel sits at 17.5 per cent of that ceiling where the CPU reaches 80.
What takes the remainder is not yet identified, and saying so is more useful
than guessing.

Accuracy in single precision is 3.7e-6 root mean square against a compensated
double-precision reference at 65536 particles, three orders of magnitude better
than the tree solver's approximation at its default opening angle. Computing the
physics in single precision costs far less accuracy than approximating it.

Phase 9 also found a defect in the CPU build that had nothing to do with the
GPU. MSVC-style release builds were compiled `/O2 /Ob1`, which inlines almost
nothing, and that made the AVX2 kernel thirteen times slower than it should be
and slower than the scalar kernel it exists to replace. It was caught because
the first GPU speedup came out at 32.7x, which was too good to believe, and
checking the CPU baseline against Phase 7's published figures settled it in one
step. No published figure was affected, since every one was taken with Clang.
The account is in
[`docs/performance/sycl_direct.md`](docs/performance/sycl_direct.md).

## Walking the tree on the GPU

Direct summation suits a GPU because every work-item does identical work in
identical order. A tree walk does not: neighbouring particles agree about most
of the tree and disagree about the part nearest them. And a GPU does not execute
work-items independently, it executes them in sub-groups of 32 that share one
instruction pointer, so a walk written as though each work-item were a thread
has every lane switched off while the others finish the nodes it did not need.

So the sub-group walks together. One node index for all 32 lanes, advanced to
the nearest node any of them still wants, with a lane that has accepted a cell
masked out until the group leaves that subtree.

| N | Independent walk | Coherent walk | Speedup | Nodes visited per lane |
| --- | --- | --- | --- | --- |
| 16384 | 5.309 ms | 1.529 ms | **3.47x** | 1.25x more |
| 131072 | 43.12 ms | 13.06 ms | **3.30x** | 1.24x more |
| 1048576 | 343.6 ms | 116.9 ms | **2.94x** | 1.19x more |

The coherent walk steps through about 25 per cent *more* nodes and takes under a
third of the time, because the nodes it adds are ones the hardware was already
executing under a divergence mask. A second session gives 3.96x, 2.92x and
2.51x, so the honest claim is about three times rather than any one of those
figures.

It computes the same answer, and that is asserted rather than hoped for. The
published warp-coherent traversals let a lane that would have accepted a cell
descend with the rest of its warp, which makes a particle's acceleration depend
on which other particles shared its warp; this one masks instead, so the test
suite can require the GPU and CPU solvers' **interaction counters to be equal**,
which says the device opened the same cells rather than that it landed somewhere
nearby (ADR-0029).

Against the solvers it has to beat, in single precision:

| N | GPU tree | CPU tree | GPU direct | vs CPU tree | vs GPU direct |
| --- | --- | --- | --- | --- | --- |
| 16384 | 2.921 ms | 25.92 ms | 7.073 ms | 8.87x | 2.42x |
| 65536 | 13.88 ms | 145.8 ms | 79.18 ms | 10.51x | **5.71x** |
| 262144 | 59.80 ms | 732.0 ms | not timed | **12.24x** | |
| 2097152 | 603.5 ms | not timed | not timed | | |

The tree overtakes direct summation on the GPU at about **9000 particles**,
against 6100 on the CPU: a tree has to reach half again the size to be worth
using on the hardware direct summation suits best. The full picture is now the
CPU direct kernel below about 2200 particles, the GPU direct kernel to about
9000, and the GPU tree solver above that.

**The largest tractable configuration is about 2.1 million particles**, at 603
ms per evaluation and 87.5 MiB of shared memory, and nothing about the device
stopped it there. The limit is on the host: **the Morton sort is 40 to 46 per
cent of every evaluation**, roughly level with the device traversal above 262144
particles and more than ten times the tree build it exists to enable. A phase
spent making the traversal three times faster has made the sort the bottleneck,
which is the most useful thing the measurement says and the first thing to fix
next (ADR-0028).

The full tables, the roofline plot, the per-worker breakdown, the error against
cost curve, the device ceilings, the sub-group width sweep and an honest account
of which of these figures reproduce and which do not are in
[`docs/performance/roofline.md`](docs/performance/roofline.md),
[`docs/performance/threading.md`](docs/performance/threading.md),
[`docs/performance/barnes_hut.md`](docs/performance/barnes_hut.md),
[`docs/performance/sycl_direct.md`](docs/performance/sycl_direct.md) and
[`docs/performance/sycl_tree.md`](docs/performance/sycl_tree.md). Reproduce
with:

```
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/orrery_roofline 8192 21
./build/release/benchmarks/orrery_threading_scaling 8192 11
./build/release/benchmarks/orrery_tree_scaling
```

and, for the GPU, with the oneAPI compiler:

```
cmake --preset sycl-single-precision -DCMAKE_CXX_COMPILER=icx-cl
cmake --build --preset sycl-single-precision
./build/sycl-single-precision/benchmarks/orrery_sycl_direct
./build/sycl-single-precision/benchmarks/orrery_sycl_tree
```

The last of those is the longest session in the project, since it drives the GPU
and all eight cores in turn at sizes where one evaluation is most of a second.
It takes an optional largest particle count, so `orrery_sycl_tree 262144` runs a
few minutes rather than half an hour.

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
| `sycl` | Release with the GPU backend. Needs the oneAPI DPC++ compiler |
| `sycl-single-precision` | The same with `float`. The configuration the GPU figures come from |
| `lint` | Debug with clang-tidy running alongside the compiler. Needs Clang |

Every one of them is exercised by continuous integration, along with a
clang-format check, on Linux with GCC and Clang, on macOS with Clang, and on
Windows with MSVC.

The GPU backend is off by default and a build without it is complete rather
than degraded: the solvers above are selected at run time and the GPU is one
more of them. Building it needs the oneAPI DPC++ compiler, which is none of the
three the project is otherwise tested with, so the compiler has to be named:

```
cmake --preset sycl-single-precision -DCMAKE_CXX_COMPILER=icx-cl
cmake --build --preset sycl-single-precision
ctest --preset sycl-single-precision
```

On Windows that is `icx-cl`, the MSVC-style driver, because CMake drives a
Windows IntelLLVM compiler with MSVC-style flags that `icpx` rejects. Elsewhere
it is `icpx`. Run the oneAPI environment script first. On a machine with no
device the suite still passes: the GPU cases skip and the discovery layer is
required to report no device rather than fail.

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
`integrators/` and `solvers/` exist so far. `backend/` holds both execution
backends: the CPU thread pool and its schedulers, and the SYCL device discovery
and unified memory the GPU solver is built on. The GPU kernel itself sits in
`solvers/` beside the CPU kernel it mirrors, because it is a summation over
pairs of particles rather than a scheduling policy (ADR-0026). `benchmarks/harness/` holds the
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
