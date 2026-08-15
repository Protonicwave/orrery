#pragma once

/// \file
/// Direct summation on a discrete NVIDIA GPU.
///
/// A third implementation of one summation, behind the interface the other two
/// are behind. ADR-0026 put the GPU behind `ForceSolver` rather than behind
/// `Executor` and predicted that a second device would not inherit anything from
/// the first beyond the discovery and allocation layers in `backend/`. This is
/// the test of that prediction, and ADR-0060 records how it came out.
///
/// ## What is the same
///
/// The kernel, almost exactly. `src/solvers/cuda_direct_kernel.cu` sets out the
/// correspondence line by line: one thread per target, sources staged through
/// shared memory a tile at a time, the self term and the padded tail masked in
/// both the mass and the squared separation. The physics is not merely the same
/// algorithm but the same function, since both kernels call
/// `core::softened_inverse_distance_cubed` rather than restating it.
///
/// ## What is different, and it is not the kernel
///
/// The memory. Phase 9's target GPU shares a memory controller with the CPU, so
/// the solver's staging step was host memory to host memory and the pointer the
/// host wrote was the pointer the kernel dereferenced (ADR-0027). None of that
/// is true here. The card has its own memory across a bus, and every evaluation
/// sends four component arrays over and brings three back.
///
/// So this solver has five timing fields where the SYCL one has three, and the
/// two extra ones are the transfers. That is not instrumentation for its own
/// sake: a reader comparing the two backends' tables needs to see which part of
/// the difference is the kernel and which is the bus, and a solver that reported
/// one total would make the question unanswerable.
///
/// The staging step also changes shape. There it copied into shared allocations
/// the kernel then read; here it assembles the four component arrays into one
/// pinned host buffer so that the transfer is a single contiguous send rather
/// than four. A discrete card charges per transfer as well as per byte, and at
/// the small end of the scaling table that fixed cost is most of the evaluation.
///
/// ## What it will and will not agree with
///
/// The same answer the other two give, to the same tolerance, and not bit for
/// bit with any of them. `tests/solvers/cuda_direct_solver_test.cpp` measures it
/// against the compensated double-precision reference in
/// `solvers/reference_kernel.hpp` with the bounds
/// `tests/solvers/sycl_direct_solver_test.cpp` uses, unchanged. Two backends
/// meeting the same bound against the same reference is the strongest statement
/// of cross-device agreement available on machines that have one device each,
/// and it is a stronger one than comparing the two devices' outputs directly
/// would be: two approximate answers agreeing says they are wrong in the same
/// way.

#include <memory>
#include <span>
#include <string_view>

#include "orrery/backend/cuda_device.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"

#ifdef ORRERY_ENABLE_CUDA

namespace orrery::solvers {

/// Where the last evaluation spent its time.
///
/// Five parts rather than the three `SyclEvaluationTimings` reports, and the two
/// extra ones are the whole architectural difference between the backends
/// expressed as numbers. ADR-0027 could argue the staging cost away because it
/// was O(N) against an O(N^2) kernel and never left the machine's own memory.
/// The transfers below are O(N) too, and they cross a bus, so the argument has
/// to be made again on different evidence: `docs/performance/cuda.md` reports
/// what fraction of an evaluation they take and at what size that fraction stops
/// mattering.
///
/// Taken unconditionally, for the reason the other backends' timing structures
/// give: a build option that switched the instrumentation off would mean the
/// configuration that reports the breakdown is not the configuration that was
/// measured.
struct CudaEvaluationTimings {
    /// Assembling the caller's positions and masses into the pinned host buffer.
    ///
    /// The counterpart of `SyclEvaluationTimings::staging_in`, and the same work:
    /// a copy from the caller's arrays into the solver's own. What differs is
    /// where the destination is, which is the next field's problem.
    backend::Duration staging_in{};

    /// Sending that buffer to the card.
    ///
    /// The cost Phase 9 did not have. Reported separately from the staging
    /// because they are different resources: one is the CPU's memory bandwidth
    /// and the other is the bus, and a reader who wants to know whether a larger
    /// configuration would be transfer-bound needs the two apart.
    backend::Duration transfer_in{};

    /// The kernel, from launch to the device reporting that it has finished.
    /// This is the figure a throughput number should be computed from, and it
    /// contains no transfer at all, which is the property using device memory
    /// rather than managed memory buys.
    backend::Duration kernel{};

    /// Bringing the accelerations back.
    backend::Duration transfer_out{};

    /// Copying them out of the pinned buffer into the caller's arrays.
    backend::Duration staging_out{};
};

/// Direct summation evaluated on a CUDA device.
///
/// Declared only where the backend is compiled, following the precedent
/// `ORRERY_HAS_AVX2_KERNEL` sets in `solvers/direct_kernel.hpp` and the SYCL
/// solvers follow. A caller that must work either way asks
/// `backend::kCudaBackendCompiled` first, which is a constant and available in
/// every build.
class CudaDirectSolver final : public ForceSolver {
public:
    /// A solver on this machine's CUDA device, or nothing.
    ///
    /// Returns null when there is no device, when the runtime cannot describe
    /// it, or when a context cannot be created on it. All three mean the same
    /// thing to a caller, which is that this machine cannot run the kernel and
    /// the CPU solver should be used instead, and none of them is an error worth
    /// an exception: a machine with no NVIDIA card is an ordinary machine.
    ///
    /// A factory rather than a constructor precisely because construction can
    /// fail for reasons that are not faults, which is the argument
    /// `SyclDirectSolver::try_create` makes at greater length.
    [[nodiscard]] static std::unique_ptr<CudaDirectSolver>
    try_create(core::Softening softening = {});

    ~CudaDirectSolver() override;

    CudaDirectSolver(const CudaDirectSolver&) = delete;
    CudaDirectSolver& operator=(const CudaDirectSolver&) = delete;
    CudaDirectSolver(CudaDirectSolver&&) noexcept;
    CudaDirectSolver& operator=(CudaDirectSolver&&) noexcept;

    /// Write the acceleration at each position into `accelerations`.
    ///
    /// The three views must describe the same particles in the same order and
    /// have the same length, as `AccelerationField` requires. Safe to call with
    /// no particles.
    ///
    /// Blocks until the device has finished and the results are back in host
    /// memory. Throws `backend::CudaError` when the runtime refuses an
    /// allocation, a transfer or the launch, which are setup and boundary
    /// failures rather than conditions a caller can act on differently.
    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "cuda-direct"; }

    [[nodiscard]] core::Softening softening() const noexcept override;

    [[nodiscard]] InteractionCount interaction_count() const noexcept override;

    void reset_interaction_count() noexcept override;

    /// What the runtime says about the device this solver runs on.
    ///
    /// Exposed because no GPU figure means anything without it, and because this
    /// backend's whole reason for existing is a comparison between two devices
    /// that a reader has to be able to identify.
    [[nodiscard]] const backend::CudaDeviceDescription& device() const noexcept;

    /// Threads per block, which is also how many sources are staged through
    /// shared memory at a time.
    ///
    /// Chosen from what the device reports rather than fixed, and reported so
    /// that a benchmark states it alongside the timing it produced. The exact
    /// counterpart of `SyclDirectSolver::tile_size`.
    [[nodiscard]] core::Index block_size() const noexcept;

    /// Where the last evaluation spent its time.
    [[nodiscard]] const CudaEvaluationTimings& timings() const noexcept;

    /// Whether the arrays the kernel reads are device memory and the buffers the
    /// host assembles are pinned, asked of the runtime rather than assumed.
    ///
    /// The mirror image of `SyclDirectSolver::uses_shared_memory`, and it exists
    /// for the opposite reason. That function is evidence that no transfer
    /// happens; this one is evidence that the transfer which does happen is the
    /// one the timings describe, rather than a page migration hidden inside the
    /// kernel column by a managed allocation. `backend/cuda_memory.hpp` sets out
    /// what the question means.
    ///
    /// False before the first evaluation, since nothing has been allocated yet.
    [[nodiscard]] bool uses_device_memory() const noexcept;

private:
    /// The runtime's handles and the device allocations are the private state,
    /// and putting them here would make `<cuda_runtime.h>` an include of every
    /// file that mentions a solver. The indirection costs one pointer hop per
    /// force evaluation, ahead of N^2 interactions on a device, which is the
    /// boundary section 3 of the implementation plan permits dispatch at and
    /// then some.
    struct Impl;

    explicit CudaDirectSolver(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
