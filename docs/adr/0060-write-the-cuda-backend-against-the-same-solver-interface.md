# ADR-0060: Write the CUDA backend to test the solver interface, not because the project needs two GPUs

- **Status:** Accepted
- **Date:** 2026-08-15

## Context

By the end of Phase 9 the project had a GPU backend, and six architecture
decision records explaining it: SYCL as the language (ADR-0025), the device
behind the solver interface rather than behind the executor (ADR-0026), the
particles staged into shared allocations (ADR-0027), the tree built on the host
and walked on the device (ADR-0028), the accepted cells masked rather than
descended into together (ADR-0029), and the node array narrowed rather than
transposed (ADR-0030).

Every one of those is written as a claim about hardware in general. ADR-0029 in
particular argues from how a machine that executes lanes in lock-step handles a
divergent branch, which is a statement about a class of processor rather than
about an Intel one. ADR-0026 goes further and makes a prediction: that a second
GPU kernel would be a `ForceSolver` too and would inherit nothing from the first
beyond the discovery and allocation layers in `backend/`.

Six records of that kind, checked against exactly one device, one runtime, one
vendor and one memory model, is not a body of decisions. It is a description of
a laptop with the word "GPU" substituted throughout, and there is no way to tell
which of the two it is by reading it more carefully. The only way to find out is
to write the thing a second time somewhere the assumptions differ, and the most
useful place to differ is the one the integrated part quietly made easy: on that
machine the CPU and the GPU read the same physical memory, and every decision
about data movement was made in a world with no bus in it.

The project does not need a second GPU backend. Nobody working on it owns an
NVIDIA card, the figures have to be taken on a hosted notebook, and the CPU and
SYCL paths already cover every machine this project is developed on. The
question is whether the interface is worth what it claims to be, and that
question cannot be answered from inside one implementation of it.

## Decision

A CUDA backend is written against the existing solver interface, as two
`ForceSolver` implementations, `solvers::CudaDirectSolver` and
`solvers::CudaTreeSolver`. It is compiled behind `ORRERY_ENABLE_CUDA`, off by
default, and a build without it is complete rather than degraded, on exactly the
terms the SYCL option already has.

It exists to test the interface. That is recorded here rather than left implicit,
because a reader who finds two GPU backends in a project with one developer is
entitled to ask why, and "for completeness" would be the wrong answer: it would
invite a third.

Four decisions follow from that purpose and are worth stating separately.

**The device compiler sees two translation units and nothing else.** SYCL is
single source through `-fsycl`, which makes every translation unit a device
translation unit; that is convenient and it costs the address sanitiser, which
cannot be applied to any file compiled that way. CUDA separates the two, so this
backend puts only the kernels in `.cu` files. Discovery, allocation, staging,
timing, counter arithmetic and both solver bodies are ordinary C++, compiled by
whichever compiler the build was already using, under this project's warning
set, seen by clang-tidy, and inside the sanitiser builds.

**The physics is annotated rather than restated.** A CUDA kernel may only call
functions marked for the device, so `core::softened_inverse_distance_cubed`, the
`Vec3` arithmetic and the two multipole terms carry `ORRERY_DEVICE`, which
expands to `__host__ __device__` under a CUDA device compiler and to nothing
under the other ten configurations this project builds in. ADR-0008 requires one
definition of the softened potential and section 3 of the implementation plan
forbids duplicated truth; a macro on nine functions is the price of keeping both.

**The two device descriptions are separate types.** SYCL reports compute units,
sub-group sizes and four unified-memory aspects; CUDA reports multiprocessors, a
warp width, a compute capability and whether the part is integrated. A single
struct holding the union would have half its fields empty whichever runtime
filled it, and a reader could not distinguish an unsupported feature from an
unasked question.

**The transfer is explicit rather than managed.** `cudaMallocManaged` would have
made the port nearly mechanical, because a single pointer valid on both sides is
what the Phase 9 solver was written around. It is not used. On a discrete card
managed memory does not remove the transfer, it performs the same transfer at a
moment nobody chose, one page fault at a time, and charges it to whichever kernel
touched the page first. The whole value of this backend is the comparison
between two memory models, and a solver that could not say what its transfer cost
would have nothing to contribute to it.

## Alternatives considered

**Not writing it.** The honest baseline, and the strongest argument against every
line of this backend: the project has a GPU path that works, and a second one is
a second thing to keep correct. It is rejected because the alternative to writing
it is not "one backend" but "six decision records nobody can check", and section
1 of the implementation plan puts engineering that survives inspection above
features. The cost is bounded and visible: two kernels, two solvers, an option
that is off, and a continuous integration job.

**Generalising the SYCL backend rather than writing a second one.** A device
abstraction layer over both, so that one solver body serves either runtime. This
is what a mature framework grows and it is the wrong move at this size, for the
reason ADR-0026 gives when it declines to build one over two SYCL kernels:
an abstraction over two implementations is indirection rather than reuse, and it
would have to be designed before the second implementation existed, which means
designing it from the first one. That is precisely the mistake this backend was
written to detect.

**A portability layer, Kokkos or Alpaka or HIP.** These solve the problem this
ADR is posing, and by construction they solve it invisibly: the whole finding
here is which decisions are vendor-specific, and a layer that hides the vendor
hides the finding. ADR-0025 rejected portability layers for Phase 9 on a
different argument, about dependency weight and about compile-time dispatch, and
both still hold. What is added here is that the answer is the point.

**Restating the force law inside the kernel.** Nine lines, compiling
immediately, no macro anywhere. Rejected because it would put a second definition
of the softened potential in a project whose headline conservation result depends
on there being one (ADR-0008), and because the duplication would be invisible:
both copies would be right on the day they were written.

**Compiling the whole backend with nvcc, so the solvers are `.cu` files too.**
The straightforward arrangement, and closer to how the SYCL backend is built. It
would put allocation, staging and timing outside the reach of clang-tidy and the
sanitisers for no benefit, since none of that code needs a device compiler.

**Making the coherence width a hardware request, as it is on SYCL.** It cannot
be: a warp is 32 lanes and there is no attribute that changes it. The narrower
widths are therefore implemented as segments of the warp's reduction, which means
the width sweeps on the two backends measure related but distinct quantities.
Recorded rather than quietly aligned, because a table that presented them as the
same measurement would be wrong in a way no reader could detect.

## Consequences

The prediction in ADR-0026 held. The two CUDA solvers derive from `ForceSolver`,
share the discovery and allocation layers in `backend/` with nothing above them,
and inherit nothing from the SYCL solvers. No change to `ForceSolver`,
`AccelerationField` or `Executor` was needed, and nothing above the solver layer
learned that a second kind of device exists.

The algorithmic decisions carried and the memory decisions did not, which is a
sharper answer than either "it all generalised" or "none of it did". ADR-0029's
divergence argument, ADR-0030's narrowed node array and ADR-0028's host-built
tree all transferred with only their spelling changed, and the suite requires the
CUDA traversals to compute the same sum as the CPU walk and as each other, which
is the same exact requirement the SYCL traversals meet. ADR-0027 did not
transfer: it is an argument about a part with no bus, and this backend has one
and has to measure what it costs.

`ORRERY_DEVICE` is now in three core headers, and it is a macro in a project that
avoids them. The reach is deliberately small and the rule for extending it is
that a function carries the annotation only if a force kernel evaluates it per
interaction. Anything that allocates, throws, or touches a container must not
have it, so that a mistake is a diagnostic at the call rather than a surprise
inside a kernel.

Continuous integration compiles the backend on a runner with no NVIDIA card,
which checks the kernels against the device compiler and checks that discovery
degrades rather than failing. It cannot check that the kernels compute anything,
and the tests that do skip themselves there. That is the same gap the SYCL job
has and it is filled the same way: by running the suite on a machine with a
device before the figures are published.

The figures come from a machine nobody working on this project owns, which is
new. Every performance document so far describes one laptop whose thermal
behaviour is understood; a hosted notebook is shared hardware of unknown
provenance. The benchmark therefore measures the device's own ceilings in the
same session as the tables, and `docs/performance/cuda.md` states the machine it
ran on rather than the part number it was promised.
