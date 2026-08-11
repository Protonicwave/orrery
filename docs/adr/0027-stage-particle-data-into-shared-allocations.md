# ADR-0027: Stage particle data into the solver's own shared allocations

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

`ForceSolver::evaluate` receives spans over the caller's arrays, which come from
`core::ParticleData` and are allocated by the cache-line allocator of ADR-0005.
The GPU kernel needs to read positions and masses and write accelerations. So
something has to connect memory the caller owns to memory a kernel can
dereference.

On a part with no separate device memory the obvious answer is that nothing has
to happen at all. Host and device share one memory controller, section 2 of the
implementation plan builds an architecture on that fact, and Phase 9's
definition of done requires the absence of a host-to-device copy to be
demonstrated. A kernel reading `ParticleData`'s arrays exactly where they lie
would be the cleanest possible demonstration of it.

SYCL has the feature that would allow it. A device with the
`usm_system_allocations` aspect can dereference a pointer from an ordinary `new`
or `malloc`, with no USM allocator involved anywhere. On such a device the
solver would capture the caller's pointers directly and there would be no
staging step to justify.

The target device does not have it. Queried through
`backend::discover_gpu_device` on the machine section 2 describes, an Intel Arc
130V on Level Zero driver 1.15.37669 reports `usm_system_allocations` false
while reporting `usm_shared_allocations`, `usm_device_allocations` and
`usm_host_allocations` true. The capability is optional in SYCL 2020 and this
driver does not implement it.

That is a measurement rather than an assumption about integrated GPUs, and it is
what forces the decision.

## Decision

`SyclDirectSolver` owns shared USM arrays sized to the padded particle count and
reused across evaluations. Each `evaluate` copies positions and masses into
them, launches the kernel, and copies the accelerations back out.

The allocation happens once per particle count rather than once per evaluation,
because a simulation calls `evaluate` millions of times at a fixed length and a
USM allocation is a driver call.

## Alternatives considered

**Capture the caller's pointers directly.** The right answer, unavailable on
this driver. Recorded here rather than dismissed because it costs nothing to
revisit: the aspect is queried on every discovery, and a future driver reporting
it true would make this decision worth superseding.

**Allocate `ParticleData`'s arrays in USM.** This would remove the copy by
moving it to construction time, which is where it belongs if the answer is going
to be paid for once. It is rejected on layering. `core/` is the bottom of the
architecture in section 3 and depends on nothing; making its allocator call into
a SYCL runtime would invert that, and would mean the particle container could
not be constructed in a build that has no GPU, no runtime and no device. Every
test and every CPU-only run would carry a device dependency to serve a backend
that is off by default.

**A separate USM-backed particle container, chosen by the caller when it intends
to use the GPU.** Layering-clean, since it would live in `backend/` or beside
the solver. Rejected because it pushes the choice of backend up into whatever
constructs the configuration, which is the opposite of the run-time solver
selection ADR-0014 and section 3 are built around: an integrator and a benchmark
should be able to swap solvers without the particle data being rebuilt.

**Map the caller's memory into the device's address space for the duration of
the call.** SYCL's buffer and accessor model does something close to this, and
it would avoid owning a second copy. It also reintroduces exactly what this
project is trying to demonstrate the absence of, since the runtime is free to
implement the mapping as a transfer and gives no way to ask whether it did.
ADR-0025 chose USM over buffers for this reason and this is where the choice is
paid off.

## Consequences

Each evaluation performs O(N) of copying against O(N^2) of kernel arithmetic, so
the staging vanishes into the noise at the sizes the GPU is worth using at, and
dominates at sizes where it is not. `docs/performance/sycl_direct.md` measures
the split rather than asserting the first half of that sentence.

The copy is host memory to host memory. Both ends are in the 32 GB the CPU and
GPU share and nothing crosses a bus, because on this part there is no bus to
cross. That is worth stating carefully, because it would be easy to read this
ADR as conceding that a host-to-device transfer happens after all. None does:
the pointer the host writes is the pointer the kernel dereferences, and
`SyclDirectSolver::uses_shared_memory` asks the runtime to confirm that rather
than asserting it.

The solver holds seven allocations sized to the padded particle count, so a
GPU solver over a million particles holds about 28 MB in single precision beyond
what the caller already holds. On a unified part that is 28 MB of the same pool
the caller's arrays came from, which is the cost this decision actually carries.

This is the decision to revisit first if a later driver reports
`usm_system_allocations`. Nothing above the solver would change, the staging and
the padded allocations would go, and the kernel would read the caller's arrays
where they lie.
