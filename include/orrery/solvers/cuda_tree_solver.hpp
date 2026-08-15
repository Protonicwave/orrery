#pragma once

/// \file
/// The Barnes-Hut traversal on a discrete NVIDIA GPU.
///
/// The second half of the experiment ADR-0060 describes. The direct solver
/// established that a kernel whose lanes all do identical work over identical
/// memory ports without an argument, which is not surprising: that is the shape
/// every wide machine is built for. The tree walk is the case that matters,
/// because the reason it is hard is a claim about how SIMD hardware executes
/// divergent branches, and a claim of that generality is only worth as much as
/// the number of vendors it has been checked against.
///
/// ## What carried over
///
/// The traversal. `src/solvers/cuda_tree_kernel.cu` sets out the three places
/// the spelling differs, and none of them is the algorithm: the coherent group
/// is a segment of a warp rather than a sub-group, the reduction is a shuffle
/// ladder rather than a group collective, and the loop condition is asked of the
/// whole warp because every lane has to reach every shuffle. The acceptance
/// test, the masking, the escape index, the summation order and the counters are
/// the same, and the two solvers call the same `monopole_acceleration` and
/// `quadrupole_acceleration` rather than restating either.
///
/// The tree is built on the host, by the same code, for the reason ADR-0028
/// gives. That reason was argued on the integrated part, where construction is
/// cheap against the traversal and the tree can be handed over without a copy.
/// Only the first half of it survives here, and `docs/performance/cuda.md`
/// reports what the second half costs on hardware where the node array has to be
/// sent.
///
/// ## What did not carry over
///
/// The staging dividend. On the integrated part, the gather into the tree's
/// sorted order and the staging into device-visible memory turned out to be the
/// same step, so the GPU tree solver paid nothing at all for the second of them:
/// the gather wrote straight into shared memory and the scatter read straight
/// out of it. That was a genuine architectural result and it is entirely a
/// property of shared memory.
///
/// Here the gather still writes straight into the pinned buffer, so that half of
/// the saving remains, and then the buffer has to be sent. What Phase 9 could
/// argue away this phase has to measure, which is why `CudaTreeTimings` has
/// seven fields where `SyclTreeTimings` has six.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "orrery/backend/cuda_device.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/cuda_kernels.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"
#include "orrery/solvers/octree.hpp"

#ifdef ORRERY_ENABLE_CUDA

namespace orrery::solvers {

[[nodiscard]] constexpr std::string_view to_string(CudaTraversal traversal) noexcept {
    return traversal == CudaTraversal::kCoherent ? "coherent" : "independent";
}

/// Where one force evaluation spent its time.
///
/// Seven parts. Six of them are the six `SyclTreeTimings` reports, doing the
/// same work in the same order, and the seventh is the transfer that hardware
/// made necessary. Keeping the six aligned is what lets the two documents put
/// their breakdowns side by side and have the comparison mean something.
///
/// Taken unconditionally, for the reason the other timing structures give: a
/// build option that switched the instrumentation off would mean the
/// configuration that reports the breakdown is not the configuration that was
/// measured.
struct CudaTreeTimings {
    /// Computing and sorting the Morton codes, on the host.
    backend::Duration ordering{};

    /// Gathering the positions and masses into the tree's order, writing
    /// straight into the pinned staging buffer. This is Phase 8's gather and
    /// half of this backend's staging at once, which is the part of the Phase 9
    /// dividend that survives a bus.
    backend::Duration gathering{};

    /// Building the octree over the sorted order, on the host.
    backend::Duration construction{};

    /// Converting the host node array into the device layout.
    ///
    /// O(number of nodes), which is a fraction of N, and reported separately so
    /// that the fraction can be read rather than assumed.
    backend::Duration node_staging{};

    /// Sending the particles and the nodes to the card.
    ///
    /// The field the other backend has no counterpart for. It is what ADR-0028's
    /// argument costs on hardware where the host and the device do not share
    /// memory, and it is reported rather than folded into the kernel so that a
    /// reader can decide for themselves whether building the tree on the host is
    /// still the right choice at a given size.
    backend::Duration transfer{};

    /// Submitting the traversal and waiting for the device. The figure a
    /// throughput number should be computed from.
    backend::Duration kernel{};

    /// Bringing the accelerations and the counters back, and scattering them to
    /// the caller's order. Phase 8's scatter and this backend's return transfer
    /// at once.
    backend::Duration scatter{};
};

/// Barnes-Hut with the traversal on a CUDA device.
///
/// Declared only where the backend is compiled, on the same terms as
/// `CudaDirectSolver`. A caller that must work either way asks
/// `backend::kCudaBackendCompiled` first.
class CudaTreeSolver final : public ForceSolver {
public:
    /// A solver on this machine's CUDA device, or nothing.
    ///
    /// Returns null on the same conditions `CudaDirectSolver::try_create`
    /// documents. None is an error and all of them mean the caller should use
    /// the CPU tree solver, which computes the same thing.
    ///
    /// `executor` schedules the host half, which is the sort and the tree build,
    /// and is referred to rather than owned. Null runs the host half on the
    /// calling thread. The result does not depend on which, since the ordering
    /// is total and the tree is a function of the sorted particles alone.
    [[nodiscard]] static std::unique_ptr<CudaTreeSolver>
    try_create(TreeParameters parameters = {}, core::Softening softening = {},
               backend::Executor* executor = nullptr);

    ~CudaTreeSolver() override;

    CudaTreeSolver(const CudaTreeSolver&) = delete;
    CudaTreeSolver& operator=(const CudaTreeSolver&) = delete;
    CudaTreeSolver(CudaTreeSolver&&) noexcept;
    CudaTreeSolver& operator=(CudaTreeSolver&&) noexcept;

    /// Write the acceleration at each position into `accelerations`.
    ///
    /// The three views must describe the same particles in the same order and
    /// have the same length. Safe to call with no particles.
    ///
    /// Blocks until the device has finished and the results are back in host
    /// memory. Throws `backend::CudaError` when the runtime refuses an
    /// allocation, a transfer or the launch.
    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "cuda-tree"; }

    [[nodiscard]] core::Softening softening() const noexcept override;

    [[nodiscard]] InteractionCount interaction_count() const noexcept override;

    void reset_interaction_count() noexcept override;

    /// The tree parameters in force, after the corrections `TreeParameters`
    /// documents.
    [[nodiscard]] const TreeParameters& parameters() const noexcept;

    /// The tree the last evaluation built, empty before the first.
    ///
    /// The host tree rather than the device copy, since the two describe the
    /// same cells and only this one can be inspected without a kernel. Exposed
    /// so that a benchmark can report the tree's shape beside its timing and a
    /// test can assert that this solver and the CPU one built the same tree.
    [[nodiscard]] const Octree& tree() const noexcept;

    /// Which traversal the next evaluation will run.
    [[nodiscard]] CudaTraversal traversal() const noexcept;

    void select_traversal(CudaTraversal traversal) noexcept;

    /// How many lanes agree to walk the tree together.
    ///
    /// The knob the divergence mitigation is measured through, and it does not
    /// mean quite what it means on the other backend. There it selects a
    /// sub-group width the hardware executes; here the hardware executes 32
    /// lanes whatever this says, and the width decides how many of them share
    /// one node index. `solvers/cuda_kernels.hpp` sets out the difference and
    /// `docs/performance/cuda.md` reports the sweep as what it is.
    ///
    /// Only 8, 16 and the device's warp width are accepted, since each is a
    /// separate instantiation of the traversal. A request outside that set
    /// becomes the warp width rather than a launch that fails, on the same terms
    /// as `DirectSolver::select_kernel`: the caller asked for a configuration,
    /// and the solver reports what it settled on.
    void select_coherence_width(unsigned width) noexcept;

    /// The width in force, which is the warp width unless a narrower one was
    /// asked for and accepted.
    [[nodiscard]] unsigned coherence_width() const noexcept;

    /// The widths this solver has a traversal for, ascending.
    [[nodiscard]] std::span<const unsigned> supported_coherence_widths() const noexcept;

    /// The block size the traversal launches with.
    ///
    /// A multiple of the warp width, since a block that is not one cannot be
    /// divided into whole warps and the shuffle ladder assumes it can.
    [[nodiscard]] core::Index block_size() const noexcept;

    /// How many nodes the traversal visited, summed over threads, since the
    /// count was last reset.
    ///
    /// The measurement the divergence mitigation turns on, and it means
    /// different things for the two traversals, which is the point of it. For
    /// the independent walk it is the sum of the lengths of the individual
    /// walks, which is the algorithm's own figure. For the coherent walk it is
    /// the segment's union walk counted once per lane, so the ratio between the
    /// two is exactly the redundancy coherence costs.
    ///
    /// Reset by `reset_interaction_count`, alongside the counters it belongs
    /// beside.
    [[nodiscard]] std::uint64_t node_visits() const noexcept;

    /// What the runtime says about the device this solver runs on.
    [[nodiscard]] const backend::CudaDeviceDescription& device() const noexcept;

    /// Where the last evaluation spent its time.
    [[nodiscard]] const CudaTreeTimings& timings() const noexcept;

    /// Whether the arrays the traversal reads are device memory and the buffers
    /// the host assembles are pinned, asked of the runtime rather than assumed.
    ///
    /// The same evidence `CudaDirectSolver::uses_device_memory` provides, asked
    /// of the node array as well as of the particles, because the node array is
    /// the allocation this solver adds.
    [[nodiscard]] bool uses_device_memory() const noexcept;

private:
    /// The runtime's handles and the device allocations, kept out of the public
    /// interface for the reason `CudaDirectSolver` gives.
    struct Impl;

    explicit CudaTreeSolver(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
