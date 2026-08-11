# ADR-0025: Write the GPU backend in SYCL

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Phase 9 adds the first backend that runs on something other than the CPU. The
question of which programming model to write it in has one answer that most
readers will assume before reaching this document, and that assumption is worth
addressing directly rather than leaving to inference.

The assumption is CUDA. It is the default vocabulary of GPU computing, most
published N-body work uses it, and a reviewer skimming a project that claims a
GPU backend will expect to find it. On this project's target hardware it is not
an option at all.

Section 2 of the implementation plan fixes the device: an Intel Arc 130V, Xe2
architecture, 7 Xe-cores, integrated on a Lunar Lake package. CUDA is NVIDIA's
proprietary model and runs on NVIDIA silicon. There is no Intel CUDA driver.
Choosing CUDA would not be a software decision here, it would be a decision to
buy a different computer and abandon the premise the project is built on, which
is that a defensible N-body simulator can be developed and measured end to end
on one consumer laptop.

So the hardware disposes of CUDA on its own. What remains is a real question,
because several models do target this device, and the answer has to hold up for
Phase 10 as well, where the tree traversal is far more demanding of whatever is
chosen than a direct kernel is.

The second constraint comes from the same section. This is an integrated GPU on
unified memory. There is no discrete video memory and no PCIe hop, so the host
and the device can address the same allocation. Phase 9's definition of done
requires the absence of any host-to-device copy to be demonstrated rather than
asserted, which means the model has to expose memory in a form where the
question can be asked and answered, not one that hides allocation behind a
runtime that may or may not be copying.

## Decision

The GPU backend is written in SYCL 2020, compiled with Intel's oneAPI DPC++
compiler, using unified shared memory for particle data. It sits behind the
`ORRERY_ENABLE_SYCL` build option, which is off by default, so that the
project's ordinary builds and its three required compilers are untouched by a
toolchain none of them provide.

## Alternatives considered

**CUDA.** Cannot execute on the target device, for the reason above. It is
recorded here as the alternative a reviewer expects rather than one that was
weighed. The portability argument against it stands independently of the
hardware: a backend written in CUDA runs on one vendor's products, whereas the
same SYCL source compiles for NVIDIA and AMD devices through other
implementations of the standard. That matters less than the hardware fact, but
it is the reason the answer would still be SYCL on a machine where both were
available.

**OpenCL.** Targets Intel GPUs well and has done for years, and the Level Zero
and OpenCL runtimes for this device come from the same driver package. Rejected
on the programming model rather than the capability. OpenCL separates host and
device code: kernels are supplied as source strings or intermediate binaries and
compiled through a C API, so the `Real` alias that ADR-0006 uses to switch the
project's precision at build time does not reach the kernel, and neither do the
softening definition in `core/softening.hpp` nor any of the shared types. Every
one of them would need a second definition inside the kernel string, kept in
step by hand. Section 3 of the plan warns specifically about two divergent
implementations of the same physics, and this alternative institutionalises it.

**Level Zero directly.** The low-level Intel API that the SYCL runtime itself
sits on. It offers the most explicit control of allocation and submission, and
would make the zero-copy demonstration trivial, since the allocation call says
in its own name where the memory lives. Rejected because everything else about
it is manual: command list construction, module loading, kernel argument
binding, and a device-side language that is still not single-source. The
explicitness that helps with one paragraph of Phase 9's evidence costs several
hundred lines everywhere else. SYCL's USM allocation functions map onto the same
driver calls, and the demonstration can be made through the runtime's own device
queries instead.

**OpenMP target offload.** Supported by the same Intel compiler, single-source,
and by far the smallest diff for a direct kernel: a handful of pragmas around
the loop that already exists. It was the strongest alternative. Rejected on
Phase 10 rather than on Phase 9. The tree traversal needs explicit control of
work-group size, sub-group behaviour and local memory to keep divergence in
hand, and expressing that through OpenMP's offload model means either compiler
extensions or accepting whatever the implementation infers. A model chosen for
the easy phase that has to be replaced for the hard one is not a saving.

**HIP.** AMD's model, which can also target NVIDIA. Not Intel. It fails the same
hardware test as CUDA while being less widely understood.

**Vulkan or DirectX compute.** Both run on this device. Both are graphics APIs
with compute attached, and the boilerplate between a shader and a result is
substantial for anyone whose problem is not rendering. Phase 12 will want
graphics, and this is not that.

**Kokkos or Alpaka.** Portability layers that would let one kernel source target
several backends, which is a genuine attraction for a project that may later
want NVIDIA hardware. Rejected as a dependency: each is a large library, and on
Intel devices each dispatches to SYCL underneath. Adding an abstraction over the
thing that would otherwise be written directly is worth it when there are
several backends to abstract over. There is one, and section 8 of the plan is
explicit about the cost of building a framework around a single implementation.

## Consequences

The project acquires a fourth compiler. Section 5 requires clean builds with
Clang, GCC and MSVC, and DPC++ is none of them, so the SYCL configuration is an
additional build rather than a fourth column in the existing matrix. The three
required compilers must continue to build the project with the option off, and
that is the configuration everything else in the repository is tested in. DPC++
is Clang-based, so the warning set in `cmake/BuildSettings.cmake` mostly applies
unchanged, but "mostly" is doing real work in that sentence and the SYCL sources
are the place to expect it to fail.

Continuous integration cannot cover this fully. GitHub's hosted runners have no
Intel GPU, so a workflow can compile the SYCL backend but cannot execute a
kernel on the hardware the phase is about. The device discovery path therefore
has to degrade cleanly to no device rather than fail, which the phase requires
anyway, and the results that matter are taken on the development laptop and
recorded in `docs/performance/`.

Single-source compilation means the kernel is parsed twice, once for the host
and once for the device, and anything it touches must be valid in both. That is
the property being bought: the softening, the scalar type and the vector maths
are the definitions the CPU backend already uses, so the two backends cannot
drift apart in the way ADR-0015 and section 3 both warn about. The cost is that
ordinary host constructs, exceptions and virtual dispatch among them, are not
available inside the kernel. The convention in section 4 that no virtual call
appears in a loop over particles was already the rule, so this constrains where
the boundary sits rather than changing the design.

Compile times for the SYCL translation units will be markedly worse than for the
rest of the project, and ahead-of-time compilation for the device makes them
worse still while removing a first-run compilation pause at runtime. That
trade-off is left to Phase 9's build integration to settle and measure.

Choosing SYCL does not by itself establish that no copy occurs. USM has three
kinds of allocation, and only device and shared allocations have the property
this architecture makes possible. Using the wrong one would produce a working
kernel that quietly copies and a speedup figure that means something other than
what it claims. The demonstration required by the phase's definition of done is
therefore about which allocation was made and what the runtime reports about it,
not about the choice recorded here.
