# The CUDA backend, and what it says about the SYCL one

Measured on a machine nobody working on this project owns, which is a first for
this directory and shapes everything below.

## What this document does not yet contain

**The figures have not been taken.** No machine this project is developed on has
an NVIDIA device, and the free-tier hosted notebook the protocol below describes
was not available while the backend was written. Rule 2.1 of this project's
standing conventions is that every number stated must be one the repository can
reproduce, and that a figure which was not measured is not written down, so the
tables in this document are empty rather than estimated.

What that leaves is not nothing, and the distinction is worth being precise
about. The claims this phase makes about the *interface* are structural, and they
are checked by the test suite and by the type system rather than by a stopwatch.
The claims it would make about *speed* are not checked at all yet.

| Claim | Established by | Status |
| --- | --- | --- |
| The backend needs no change to the solver interface | The code compiles against `ForceSolver` unchanged | Established |
| The two CUDA traversals compute the same sum as each other | `tests/solvers/cuda_tree_solver_test.cpp`, exact counter equality | Established on a device |
| The CUDA tree walks the same cells as the CPU tree | The same file, exact counter equality | Established on a device |
| The kernels agree with the compensated reference | `tests/solvers/cuda_direct_solver_test.cpp`, the SYCL bounds unchanged | Established on a device |
| The kernels pass the device compiler | The `cuda` job in continuous integration | Established |
| Discovery degrades rather than failing without a device | The same job, which has no device | Established |
| How fast any of it is | `benchmarks/cuda_scaling.cpp` | **Not taken** |
| Whether the coherent traversal wins on a second vendor | The same benchmark | **Not taken** |

"Established on a device" means the case exists, is exact rather than tolerant,
and skips itself where there is nothing to run it on. Those cases have not run
anywhere yet, and the section at the end says what has to happen before this
document can say they have.

## What the backend is

Two `ForceSolver` implementations behind the interface ADR-0026 introduced:
`CudaDirectSolver` and `CudaTreeSolver`, mirroring `SyclDirectSolver` and
`SyclTreeSolver`. It is built by the `cuda` and `cuda-single-precision` presets,
off by default, and a build without it is complete rather than degraded.

ADR-0060 records why a second GPU backend exists in a project that needs one. The
short version is that six architecture decision records from Phase 9 are written
as claims about hardware in general, and a claim checked against a single vendor
cannot be distinguished from a description of that vendor.

## What carried over unchanged

The algorithm, in both solvers, to the point where the two kernel files are
paraphrases of each other.

**Direct summation.** One thread per target, sources staged through shared memory
a tile at a time, the self term and the padded tail masked in both the mass and
the squared separation. `sycl::nd_item::get_global_id` becomes
`blockIdx * blockDim + threadIdx`, `sycl::local_accessor` becomes dynamic shared
memory, `sycl::group_barrier` becomes `__syncthreads`. Nothing else moved.

**The tree traversal.** The acceptance test, the escape index, the skip mask, the
summation order and the counters are the same. ADR-0029's argument, that a lane
sitting masked out during another lane's node is a lane doing nothing, is an
argument about lock-step execution and it transfers without amendment.

**The node layout.** ADR-0030 narrowed the tree node's three index fields to 32
bits, arguing from cache line sizes and from the number of particles a machine
can hold. Neither is a property of a vendor, and the CUDA node is the same
structure.

**The physics itself,** in the strongest sense available: both backends call
`core::softened_inverse_distance_cubed`, `monopole_acceleration` and
`quadrupole_acceleration` rather than restating them. They compute the same
function and not merely the same algorithm.

## What did not carry over

The memory, and one knob.

**ADR-0027 does not transfer.** It argues that staging the particles into shared
allocations is O(N) against an O(N^2) kernel and therefore stops mattering, and
that argument rests on the staging being host memory to host memory on a part
where the CPU and GPU share a memory controller. A discrete card has its own
memory across a bus. The copy is real, it cannot be argued away, and this backend
therefore reports five timing fields where the SYCL direct solver reports three,
and seven where the SYCL tree solver reports six. The extra ones are the
transfers.

`cudaMallocManaged` would have restored the Phase 9 shape almost exactly, and it
is deliberately not used: on a discrete card it performs the same transfer at a
moment nobody chose, one page fault at a time, and charges it to whichever kernel
touched the page first. `tests/backend/cuda_memory_test.cpp` requires the
kernels' arrays to report as device memory and the staging buffers as pinned
host memory, and requires neither to be managed, so a later change that hid the
transfer inside the kernel column fails rather than passing slightly differently.

**The staging dividend is halved.** Phase 9 found that the tree solver's gather
into Morton order and its staging into device-visible memory were the same step,
so the GPU tree solver paid nothing for the second of them. Half of that survives
here: the gather still writes straight into the buffer the transfer reads from,
so the copy is the one the algorithm needed anyway, and only the transfer itself
is new.

**The coherence width means something different.** On SYCL the sub-group width is
requested through a kernel attribute and the device compiles for the width it was
asked for, so 8, 16 and 32 are three hardware configurations. A warp is 32 lanes
and no attribute changes that. The narrower widths here are segments of the
warp's reduction: the same 32 lanes are executing, told to agree in smaller
groups. The sweep therefore answers a related question rather than the same one,
and the two documents' width columns must not be read as one table.

## What is checkable without a device, and is checked

The `cuda` job in continuous integration runs on a machine with no NVIDIA card,
which is deliberate and is the same arrangement the SYCL job has. It establishes
three things.

The kernels pass the device compiler, including the shuffle ladder, the
templated coherence widths and the dynamic shared memory, all of which are
constructs an ordinary C++ compiler would never see.

The host halves compile under this project's full warning set. That is a larger
share of this backend than of the SYCL one: only the two `.cu` files are outside
it, where `-fsycl` puts every translation unit that mentions a solver outside the
sanitiser builds entirely.

And discovery answers rather than throwing. Every CUDA case in the suite skips
itself where there is no device and requires the discovery layer to report so, so
a change that turned a missing card into an error fails in this job rather than
on somebody's notebook.

## The sanitiser position, which is better here

`sycl_direct.md` records that the address sanitiser cannot be combined with the
SYCL backend at all, because `-fsycl` compiles every translation unit for the
device and the device target rejects the sanitiser flag. The whole of the SYCL
backend is therefore outside the sanitiser run.

CUDA does not have that problem, because only a `.cu` file needs the device
compiler. The solvers, the allocations, the staging and the counters are ordinary
translation units and are sanitised in the ordinary builds like everything else.
What is not sanitised is the two kernels, which is the smallest gap this vendor's
toolchain allows.

NVIDIA ships `compute-sanitizer`, a separate tool that instruments device code at
run time, and it has not been evaluated here. A session that takes the figures
below should run it once over the test suite, and this document should record
what it found.

## Taking the figures

The benchmark is `benchmarks/cuda_scaling.cpp`, built as `orrery_cuda_scaling`.
It follows the protocol of ADR-0019 exactly as every other benchmark here does: a
settling warm-up whose trials are discarded, timed trials reported as a median
with an interquartile spread beside it, a cool-down between configurations, and a
thermal canary bracketing the session.

It produces four tables. Direct summation scaling against this machine's CPU,
with the movement cost in a column of its own. Accuracy against the compensated
double-precision reference in `solvers/reference_kernel.hpp`. The tree traversal,
both walks at every coherence width, with the node visits each makes beside the
time each takes. And the device's own three ceilings, measured in the same
session by the probes in `benchmarks/cuda_probes.cu`.

On a machine with the toolkit and a card:

```
cmake --preset cuda-single-precision
cmake --build --preset cuda-single-precision
ctest --preset cuda-single-precision
./build/cuda-single-precision/benchmarks/orrery_cuda_scaling
```

Run the suite first and read its result. Every exact-agreement case in it is a
stronger statement than any timing, and a timing taken from a traversal that
fails them is a measurement of the wrong program.

The kernels are compiled for the architectures in `ORRERY_CUDA_ARCHITECTURES`,
which defaults to 75, Turing, because that is what a free-tier T4 is. A different
card needs its own: `-DORRERY_CUDA_ARCHITECTURES=86` for Ampere, `89` for Ada.
Compiling for an architecture the card does not have produces a launch the driver
refuses, and the device table the benchmark prints reports the compute capability
it found so that the refusal can be diagnosed from the output.

### On a hosted notebook

The intended machine is a free-tier T4 through Colab or Kaggle, which is what
makes this backend measurable at all without buying hardware. Three things about
that arrangement have to reach whoever reads the figures.

It is shared. The host processor is a small allocation of a machine running other
tenants' work, so the CPU column in the scaling table is not the eight-core
laptop the Phase 9 tables compare against, and the two speedup columns are not
the same measurement. The benchmark prints the machine state above the tables for
that reason and this document must quote it.

It is unaccountable. A hosted card may be power-limited or thermally limited in
ways nothing reports, which is why the three ceiling probes run in the same
session as the tables rather than being taken from a specification sheet. The
project's rule since Phase 7 is that a figure is a fraction of a limit measured
on the machine in front of you, and here that rule is doing real work rather than
being a formality.

And it is not reproducible on demand. A session can be reclaimed part way
through, and a different session may be given a different card. Following
`roofline.md`'s practice, the figures published here should come from a session
whose canary stayed small, and should be checked against at least one independent
session before any of them is quoted in the README.

## What the tables will have to say

Three questions, written down before the answers are known so that the document
cannot be arranged around whatever came out.

**Where the crossover is.** The integrated part overtook the CPU at about 2200
particles, held back by a launch cost of roughly 150 microseconds. A discrete
card adds a transfer to that fixed cost and brings far more arithmetic, so the
crossover could move in either direction and the reason will be in the movement
column rather than in the kernel one.

**Which ceiling binds.** On the CPU the direct kernel is bound by the square root
and division in every interaction, and reaches 80 per cent of that narrow ceiling.
On the integrated GPU it was bound by neither that nor multiply-add, and Phase 9
recorded honestly that it had not identified what was taking the remainder. A
second device with a different ratio between the two arithmetic units is the
cheapest available evidence about which of the two candidate explanations, the
barriers or the shared memory traffic, is the right one.

**Whether the coherent traversal still wins.** On the integrated GPU it is faster
while visiting more nodes, which is the result that makes ADR-0029 worth having.
If it holds on a second vendor the decision is about lock-step execution; if it
does not, the decision is about Intel and the ADR needs amending. Either outcome
is worth the phase, and the outcome that would be worth nothing is not measuring
it.

## Until then

The README's results table is unchanged, and deliberately: it gathers measured
headline figures, and this backend has none yet. The presets and the option are
documented there because they exist; no row was added because no row would be
true.
