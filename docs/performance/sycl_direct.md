# The GPU direct kernel, against the CPU it shares its memory with

What Phase 9 measured, on the machine section 2 of the implementation plan
describes.

The short version, in four findings.

The integrated GPU computes direct summation **4.1 times faster** than all eight
CPU cores running the AVX2 kernel, at 65536 particles in single precision. It
overtakes the CPU at about **2200 particles** and is slower below that.

It reaches **1174 Gflop/s**, which is 42 per cent of the multiply-add ceiling
measured on the device itself. Unlike the CPU kernel, it is not limited by the
square root and division in every interaction: it sits at 17.5 per cent of that
ceiling, where the CPU kernel reaches 80 per cent of its own.

**No host-to-device copy occurs**, and that is demonstrated rather than asserted.
The staging the solver does perform costs between 0.1 and 1.0 per cent of an
evaluation and is host memory to host memory.

And the phase found a defect that had nothing to do with the GPU: MSVC-style
release builds were **inlining nothing**, which made the project's AVX2 kernel
thirteen times slower than it should be and slower than the scalar kernel it
replaces. That is written up at the end, because the way it was found matters
more than the fix.

## The machine and the device

Taken with the `sycl-single-precision` preset, compiled by the oneAPI DPC++
compiler 2025.1.1, on the CPU described in
[`roofline.md`](roofline.md).

| Property | Value |
| --- | --- |
| Device | Intel Arc 130V GPU, Xe2 |
| Runtime | oneAPI Unified Runtime over Level Zero |
| Driver | 1.15.37669 |
| Compute units | 64 |
| Sub-group width | 32 |
| Maximum work-group | 1024 |
| Local memory | 128 KiB per work-group |
| Global memory | 16870 MiB, shared with the host |
| `fp64` | yes |
| Shared USM | yes |
| System USM | **no** |
| Tile size chosen | 256 |

Two of those rows are worth pausing on, because both contradict what is usually
assumed about integrated Intel GPUs.

The device **does** report `fp64`. The solver therefore constructs in a
double-precision build rather than declining, and the choice between precisions
is a throughput question rather than a capability one.

The device **does not** report `usm_system_allocations`, which means a kernel
cannot dereference a pointer from an ordinary `new`. That single measured fact
is what forces the staging step, and ADR-0027 rests on it rather than on a
general claim about integrated parts.

Note also that the runtime reports 64 compute units where section 2 of the plan
records 7 Xe-cores. Both are correct: the runtime counts the vector engines
inside the cores. Neither number is wrong and they should not be reconciled by
assuming one is.

## Scaling against the CPU

Both solvers run direct summation over the same Plummer spheres with the same
softening, so what is compared is two processors rather than two algorithms. The
CPU side is the fastest thing this project has: the AVX2 kernel on the
work-stealing executor across all eight cores.

| N | GPU ms | spread | kernel ms | stage ms | stage % | CPU ms | Speedup | Ginteract/s | Gflop/s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1024 | 0.178 | 1.5% | 0.180 | 0.002 | 1.0% | 0.119 | 0.67x | 5.83 | 117 |
| 2048 | 0.351 | 2.3% | 0.331 | 0.002 | 0.5% | 0.334 | 0.95x | 12.65 | 253 |
| 4096 | 0.715 | 0.3% | 0.696 | 0.003 | 0.4% | 1.161 | 1.62x | 24.11 | 482 |
| 8192 | 1.555 | 1.0% | 1.507 | 0.005 | 0.3% | 4.511 | **2.90x** | 44.52 | 890 |
| 16384 | 7.018 | 1.8% | 6.877 | 0.018 | 0.3% | 17.782 | 2.53x | 39.03 | 781 |
| 32768 | 21.473 | 3.5% | 23.060 | 0.043 | 0.2% | 68.926 | 3.21x | 46.56 | 931 |
| 65536 | 75.387 | 1.8% | 75.417 | 0.115 | 0.2% | 308.518 | **4.09x** | 56.95 | 1139 |
| 131072 | 297.057 | 2.0% | 292.752 | 0.246 | 0.1% | not timed | | 58.68 | **1174** |

The speedup column divides CPU total by GPU total, so the staging counts against
the GPU. The rate columns come from the kernel time alone, because that is what
the hardware did.

**The crossover is at about 2200 particles.** Below it the GPU loses, and at 1024
it loses by a third. That is not a defect and it is worth stating plainly: a
kernel launch has a fixed cost of roughly 150 microseconds on this runtime, and
below a few thousand particles there is not enough arithmetic to amortise it.
A simulation of a thousand bodies should use the CPU.

The kernel time is not monotone in the obvious way. The rate climbs to 44.5
Ginteract/s at 8192, falls to 39.0 at 16384, then climbs again. The dip is
reproducible across all four sessions taken and is where the padded launch
geometry fits the device least well; chasing it is Phase 10's business rather
than this phase's.

Direct summation on the CPU is not timed above 65536 for the reason
[`barnes_hut.md`](barnes_hut.md) gives: one evaluation there is already four
billion interactions and a row of trials is minutes.

## Where the staging goes

ADR-0027 argues that copying the particles into shared memory is O(N) against a
kernel that is O(N^2), and therefore stops mattering at the sizes a GPU is worth
using at. The `stage ms` and `stage %` columns are that argument turned into a
measurement, and they support it: staging falls from 1.0 per cent of an
evaluation at 1024 particles to 0.1 per cent at 131072, which is the
one-over-N behaviour the argument predicts.

It is worth being exact about what that copy is, because it would be easy to
read this section as conceding a host-to-device transfer. It is not one. Both
ends are in the 32 GB the CPU and GPU share, nothing crosses a bus, and the
pointer written by the host is the pointer the kernel dereferences. What forces
it is the absence of `usm_system_allocations`, not the presence of a device
memory to copy into.

## That there is no host-to-device copy

Phase 9's definition of done asks for this to be demonstrated rather than
asserted, so it is asserted by test in three ways rather than by paragraph.

`tests/backend/sycl_usm_test.cpp` writes an allocation from the host through an
ordinary pointer, has a kernel read and modify it through the same pointer
value, and reads the results back on the host, without calling any copy, map or
transfer operation at any point. It then asks the runtime what kind of pointer
it is holding and requires the answer to be shared, and separately requires an
ordinary heap pointer to answer otherwise, so the query is discriminating rather
than agreeable.

`SyclDirectSolver::uses_shared_memory` asks the same question of the arrays the
force kernel actually reads, and `tests/solvers/sycl_direct_solver_test.cpp`
requires it after a real evaluation.

The negative result matters as much as the positive one. If the solver had
quietly allocated device memory, the pointer query would say `device`, the
host's dereference would be undefined, and the test would fail rather than pass
slightly slower.

## Against the device's own ceilings

Phase 7 established the rule: a performance figure is a fraction of a limit
measured on the machine in front of you, not of a manufacturer's peak. Those
were CPU limits, so this phase measures the same two probes on the device, plus
the divide and square root probe that turned out to be the binding one on the
CPU.

| Probe | Measured |
| --- | --- |
| Fused multiply-add | 2781 Gflop/s |
| Divide and square root | 672 Gop/s |
| Read bandwidth | 28.6 GB/s |

Against those, the kernel's best row:

| Ceiling | Achieved | Fraction |
| --- | --- | --- |
| Multiply-add | 1174 Gflop/s | **42.2%** |
| Divide and square root | 117 Gop/s | 17.5% |

**This is where the GPU differs most from the CPU, and it is the most
interesting result in the phase.** On the CPU, the direct kernel is bound by the
square root and division in every interaction: those retire on a unit with a
twenty-seventh of the throughput of the multiply-add pipelines, and the
vectorised threaded kernel reaches 80 per cent of that narrow ceiling. On this
GPU the ratio between the two ceilings is 4.1 rather than 27, so the divide unit
is comparatively far stronger, and the kernel sits at only 17.5 per cent of it.

So the GPU kernel is not divide-bound, and at 42 per cent of the multiply-add
ceiling it is not multiply-add-bound either. Something else is taking the
remainder, and the honest position is that this phase has not identified it. The
two candidates are the two work-group barriers per tile, which serialise every
work-item in a group against the slowest, and the local memory traffic that the
tiling trades global traffic for. Distinguishing them needs sub-group level
instrumentation, which is Phase 10's territory.

The arithmetic intensity is about **320 flop per byte** of source data, because
each work-group reads every source once rather than each work-item doing so:
that factor of the tile size is the entire point of staging through local
memory. At that intensity, even the modest bandwidth measured above could feed
far more arithmetic than the device can perform, so this kernel is not
bandwidth-bound and the roofline's diagonal is nowhere near it.

The bandwidth probe deserves a caveat rather than a defence. 28.6 GB/s is well
below the 95.7 GB/s the CPU sustains on the same physical memory, and that is
more likely to be a limitation of the probe, whose pages are first touched by
the host and whose access pattern was written for coalescing rather than for
depth, than a property of the device. It is reported as measured. It does not
affect any conclusion here, because the kernel is three orders of magnitude away
from being bandwidth-bound, but it should not be quoted as this GPU's memory
bandwidth and a later phase that cares about bandwidth should measure it
properly.

## Accuracy

Measured against `solvers/reference_kernel.hpp`, which sums the same softened
force law with compensation in double precision, rather than against the CPU
solver. Two approximate answers agreeing would say they are wrong in the same
way, not that either is right.

| N | Worst relative error | Root mean square |
| --- | --- | --- |
| 4096 | 2.63e-6 | 9.32e-7 |
| 16384 | 4.71e-6 | 1.79e-6 |
| 65536 | 1.10e-5 | 3.71e-6 |

These are single-precision figures and they are what single precision costs, not
what the GPU costs. The error grows roughly as the square root of N, which is
what reassociated accumulation of N terms does, and the GPU agrees with the CPU
solver to 1.9e-6 at 2048 particles, which is the same order as each one's
distance from the truth.

For comparison, the Barnes-Hut solver at its default opening angle has a root
mean square error of 1.9e-3, three orders of magnitude larger. Approximating the
physics costs far more accuracy than computing it in single precision does.

The mass-weighted sum of the accelerations, which is exactly zero for direct
summation because every pair is computed from both ends, comes back at 6.2e-9 of
the scale of the terms that formed it. That is round-off, and it is a sensitive
check: a missing self-mask or a stale tile would show up there immediately.

## How much of this is reproducible

Four sessions were taken. Three are usable and one is not, and saying which is
the point of this section.

Per-row interquartile spreads in the quoted session run from 0.3 to 3.5 per
cent, so the rows can be quoted to three figures. The thermal canary moved 10.8
per cent across them, which is higher than Phase 7's 5.2 and Phase 8's 1.7, and
that is expected rather than alarming: this is the first benchmark in the
project to load the GPU and the CPU hard in the same session, and on a Lunar
Lake part they share one package and one power budget, so each heats the other.

The stronger evidence is agreement between independent sessions. Three sessions
run minutes apart give 4.09x, 4.20x and 4.06x at 65536 particles, and their GPU
kernel times agree within 4 per cent. That is direct reproducibility rather than
an inference from a canary.

The unusable session is worth recording. The first run of this benchmark used
the harness's default 750 millisecond cool-down and ended with a canary
reporting the machine **191 per cent** slower than it started. Its per-row
spreads were all under 6 per cent, so nothing about the rows themselves looked
wrong, and it reported 32.7x at 65536 rather than 4.1x. Most of that gap was the
inlining defect below, but a real part of it was a hot machine inflating the CPU
rows measured late in the session. The remedy was the one
[`roofline.md`](roofline.md) prescribes and the same one it used: lengthen the
cool-down, repeat the session, and quote the repeat.

Reproduce with:

```
cmake --preset sycl-single-precision -DCMAKE_CXX_COMPILER=icx-cl
cmake --build --preset sycl-single-precision
./build/sycl-single-precision/benchmarks/orrery_sycl_direct
```

Let the machine idle for a few minutes first. A session started immediately
after a full rebuild measures the cooling system.

## A note on the sanitiser builds

`barnes_hut.md` records which sanitisers this machine can run: the address
sanitiser does build and pass here once the Microsoft container annotations are
disabled, and the undefined-behaviour sanitiser does not build at all because
its runtime is compiled against a different C runtime from the one this
toolchain links.

Phase 9 adds a third case, and it is a hard limit rather than a missing
component. The address sanitiser cannot be combined with the SYCL backend at
all:

```
icx-cl: error: ignoring '-fsanitize=address' option as it is not currently
supported for target 'spir64-unknown-unknown'
```

`-fsycl` compiles every translation unit for the device as well as the host, so
the device target sees the sanitiser flag and rejects it, and there is no way to
apply the flag to only the host half from the build system. The address
sanitiser therefore covers the whole project **except** the SYCL translation
units, which is where it stands today.

The gap is smaller than it sounds and it is worth being precise about its shape.
`backend/sycl_device.cpp` and `backend/sycl_usm.cpp` compile in an ordinary
build too, as the stubs that report no device, so their host halves are
sanitised in every other configuration. What is never sanitised is the kernel in
`solvers/sycl_direct_solver.cpp` and the USM allocation paths, which are exactly
the parts most worth instrumenting: the kernel does index arithmetic over padded
ranges and the allocations are raw pointers by necessity.

Two things stand in for it. The padded ranges are allocated rather than merely
tolerated, so the tail work-items read and write inside the allocation instead
of past it, and that is the reason `ensure_capacity` sizes to the padded count
rather than the particle count. And Intel ships a device-side sanitiser of its
own, enabled through a runtime environment variable rather than a compiler flag,
which was not evaluated here. A later phase that touches the kernel should
evaluate it.

## The defect this phase found in the CPU

The first benchmark session reported the GPU as 32.7 times faster than the CPU.
That number was not questioned because it was disappointing; it was questioned
because it was too good. An integrated GPU sharing a memory controller and a
power budget with the CPU it is being compared against should not be thirty
times faster than eight vectorised cores.

Checking the CPU baseline against Phase 7's published figures settled it
immediately: this benchmark measured 47.2 ms for 8192 particles in single
precision where Phase 7 records about 7 ms for the same kernel. The GPU was
fine. The CPU baseline was broken.

The cause is in neither the GPU code nor the kernel. CMake's MSVC-style
`RelWithDebInfo` is `/O2 /Ob1`, and `/Ob1` inlines only functions marked
`inline`. The AVX2 kernel is built from small helper functions wrapping the
intrinsics, which is exactly the structure ADR-0018 chose, and under `/Ob1` every
one of them became a function call in the innermost loop.

Measured on this machine at 8192 particles in single precision:

| Configuration | AVX2, 1 thread | AVX2, 8 threads | FMA ceiling probe |
| --- | --- | --- | --- |
| icx-cl with `/Ob1` | 222 ms | 49.1 ms | 47 Gflop/s |
| icx-cl with `/Ob2` | 18.5 ms | 4.67 ms | 571 Gflop/s |
| Clang, for reference | 24.0 ms | 6.24 ms | 532 Gflop/s |

Under `/Ob1` the vector kernel was slower than the scalar kernel it exists to
replace, and the harness's own measurement of the machine's arithmetic ceiling
was out by a factor of twelve.

Nothing warned about any of this. The build succeeded with warnings as errors,
the CPUID check passed, the solver correctly reported that it was running the
AVX2 kernel, and every figure taken that way was wrong by an order of magnitude
while looking entirely ordinary.

No previously published figure is affected. Every number in this directory was
taken with Clang, which uses `-O2` and was never subject to it. The defect was
latent for MSVC-style builds, which the project has always supported and had
never benchmarked with, and it surfaced only because Phase 9 introduced a fourth
compiler that happens to be MSVC-style.

Two smaller findings came out of the same investigation.

The oneAPI compiler defaults to `-fp-model=fast`, which is precisely the
relaxation ADR-0020 declines. Clang, GCC and MSVC are all strict unless asked
otherwise, so that ADR had never needed enforcing before. Under the default,
eight tests fail, among them the two that exist to assert that compensated
summation keeps digits an ordinary total loses: compensation is algebraically a
no-op, so a compiler permitted to reassociate may simply delete it, and the
reference this project measures every approximation against stops being a
reference.

And with `/fp:fast` the *scalar* kernel becomes four times faster, because the
compiler auto-vectorises it. That would silently replace the baseline the vector
kernel is measured against with a different vector kernel, which is the second
of the three reasons ADR-0018 gives for not building the project with
`-march=native`. Keeping `/fp:precise` is therefore right twice over.
