# ADR-0026: Put the GPU behind the solver interface, not behind the executor

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Section 3 of the implementation plan says a GPU implementation is "a backend
behind the existing solver interface, not a parallel copy of the solver". That
settles what must not happen. It does not settle which interface, because by
Phase 9 the project has two that could plausibly carry a device.

The lower one is `backend::Executor`, introduced in Phase 6 and recorded in
ADR-0017. A solver hands it a range of particle indices and a callable, and the
executor decides how to divide the range between workers. The direct solver was
deliberately written so that this division changes nothing about the answer, and
the tree walk of Phase 8 has the same property. Every parallel loop in the
project already goes through it.

The higher one is `solvers::ForceSolver` from ADR-0014, which asks a whole
question rather than a piece of one: given these masses at these positions, what
is the acceleration of each.

Putting the GPU behind the executor is the more attractive of the two on first
inspection, and it is worth saying why. It would make the device a scheduling
choice rather than an algorithm choice, so every existing solver would gain a
GPU path at once, including the tree walk that Phase 10 has to write a GPU
traversal for. One kernel, written once, reachable from everything. The layering
would also be tidier, since `backend/` is where section 3 says execution
belongs.

It cannot work, and the reason is worth stating precisely rather than as a
generality about GPUs.

`Executor::run` takes a `core::FunctionRef`, which is a pointer to a host
function together with a pointer to captured host state. Running it on a device
would mean the device executing host machine code through a host function
pointer. There is no SYCL feature for that and there is no version of the
hardware where it is possible. The callable the direct solver passes is a
particularly clear case: it calls through `AccumulateRange`, a function pointer
that on this machine resolves to a translation unit compiled with AVX2
intrinsics.

The obstacle is not an inconvenience of the current design. It is the
single-source property ADR-0025 selected SYCL for: device code has to be visible
to the device compiler at the point the kernel is written, and a type-erased
callable chosen at run time is exactly what makes code invisible to it.

## Decision

The GPU direct kernel is a `ForceSolver`, `solvers::SyclDirectSolver`, holding
its own queue and its own allocations. It does not use an `Executor` and no
`Executor` implementation dispatches to a device.

`backend/` gains the parts of the SYCL backend that are genuinely about
execution resources rather than about gravity: device discovery
(`backend/sycl_device.hpp`) and unified memory ownership
(`backend/sycl_usm.hpp`). The kernel itself lives in `solvers/` beside the CPU
kernel it mirrors, because it is a summation over pairs of particles and that is
what `solvers/` is for.

## Alternatives considered

**A SYCL executor.** The layering above. Impossible for the reason given: a host
`FunctionRef` cannot be invoked on a device.

**Templating the solvers on their backend, so the loop body is visible to the
device compiler.** This is the shape that would make an executor-like seam
work, and it is what the portability layers rejected in ADR-0025 do internally.
It would mean every solver becoming a template, the kernel bodies moving into
headers, and the run-time selection the project relies on for benchmarking being
replaced by compile-time selection. ADR-0006 declined to template the solvers on
their scalar type for weaker versions of the same reasons, and section 3 permits
virtual dispatch precisely so that a benchmark can swap implementations from a
command-line flag.

**A second, narrower device-side executor interface taking something the device
compiler can see, such as a kernel functor type.** Coherent, and it is roughly
what a mature framework would grow. Rejected as premature: there is one GPU
kernel today and Phase 10 will add a second. An abstraction over two kernels
that share a queue and nothing else would be indirection rather than reuse, and
section 8 of the plan warns specifically about building a framework around a
single implementation.

**Making `SyclDirectSolver` derive from `DirectSolver` to share the staging and
the counters.** Rejected because the shared part is a dozen lines of copying and
a closed-form counter update, and the unshared part is the entire kernel.
Inheritance between two concrete solvers would also put a virtual call in a
place the plan reserves for boundaries.

## Consequences

Phase 10's GPU tree traversal is a `ForceSolver` too, and will not inherit
anything from this one beyond the discovery and allocation layers in `backend/`.
That is the honest position: the two kernels share a device, not an algorithm.

The two direct solvers are two implementations of one summation, which is the
duplication section 3 warns about. The mitigation is the same one ADR-0018
applied to the AVX2 kernel: they are not independent statements of the physics.
Both call `core::softened_inverse_distance_cubed`, both work in the units of
ADR-0007, both report interactions through the same closed form, and
`tests/solvers/sycl_direct_solver_test.cpp` measures both against the
compensated reference rather than against each other.

A caller that wants the GPU asks for it by constructing a different solver,
which is the same thing it already does to choose between direct summation and
Barnes-Hut. Nothing above the solver layer changes, and the integrators do not
learn that a device exists.

The GPU solver cannot be combined with the work-stealing executor to use the CPU
and GPU together on one evaluation. That is a real capability this decision
forecloses, and it is not in any phase of the plan: splitting one force
evaluation across two processors with different throughputs needs a
load-balancing scheme of its own, and the honest place for it would be a
composite `ForceSolver` holding both, which this decision leaves open.
