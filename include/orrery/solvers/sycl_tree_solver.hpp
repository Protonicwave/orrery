#pragma once

/// \file
/// The Barnes-Hut traversal on the integrated GPU, which is the hardest thing
/// this project asks of the device.
///
/// Phase 9 put direct summation on the GPU and it went well, for a reason that
/// is worth stating before this file explains why it does not apply here: every
/// work-item did identical work in identical order over identical memory. A
/// wide SIMD machine is built for exactly that. A tree walk is the opposite. Two
/// neighbouring particles agree about most of the tree and disagree about the
/// part of it nearest them, so their walks visit overlapping but different sets
/// of nodes, in different numbers, for different lengths of time. Section 7 of
/// the implementation plan calls this the phase where naive GPU ports fail, and
/// the mechanism of that failure is the subject of this file.
///
/// ## What actually goes wrong
///
/// A GPU does not execute work-items independently. It executes them in
/// sub-groups of a fixed width, 32 on this device, which share one instruction
/// pointer. When the lanes of a sub-group take different branches the hardware
/// runs both sides and masks off the lanes that did not want each, so the cost
/// of a divergent branch is the sum of its sides rather than the maximum. A
/// walk written as though each work-item were a thread therefore does not cost
/// what its own node count suggests. It costs what the whole sub-group's node
/// counts add up to, because every lane is switched off while the others finish
/// the nodes it did not need, and switched on again while they wait for the
/// nodes it did.
///
/// That cost is already being paid. The lanes are already stepping through the
/// union of their walks. What the independent walk does not do is *use* the
/// time: a lane sitting masked out during another lane's node is a lane doing
/// nothing.
///
/// ## The mitigation
///
/// So the sub-group walks the tree together. One node index for the whole
/// sub-group, advanced by agreement: the group descends into a node when any
/// lane in it needs to, and skips the node's subtree when none does. Each lane
/// then decides for itself what to do at the node the group has arrived at, and
/// a lane that accepted an ancestor of that node contributes nothing until the
/// walk leaves the subtree it accepted.
///
/// The trade is exact and measurable in both directions. Against it, the
/// sub-group visits the union of its lanes' walks, so a lane sees nodes its own
/// walk would have skipped and the total node visits rise. In favour of it, the
/// visits are the ones the hardware was executing under a mask anyway, and the
/// walk now has one instruction stream rather than 32 interleaved ones: the
/// acceptance test is evaluated for every lane at once, the node is read from
/// memory once for the whole sub-group instead of 32 times from 32 addresses,
/// and the escape pointer is followed once.
///
/// Both halves are measured rather than argued.
/// `docs/performance/sycl_tree.md` reports the node visits each traversal makes
/// and the time each takes, and the interesting result is that the coherent walk
/// is faster while visiting more nodes.
///
/// ## What is not given up
///
/// The answer. This is the part that separates the scheme here from the
/// published warp-coherent traversals it otherwise resembles, and ADR-0029 sets
/// it out: those make the whole warp descend when any lane needs to and let the
/// lanes that would have accepted the cell descend with it, which computes a
/// *different*, more accurate sum whose value depends on which particles shared
/// a warp. This traversal masks instead. A lane that accepts a cell adds the
/// cell's term and then contributes nothing until the group leaves that
/// subtree, so each target is summed over exactly the nodes its own walk would
/// have visited, in the same order.
///
/// The consequence is that the two traversals in this file are the same
/// function of the input, and `tests/solvers/sycl_tree_solver_test.cpp` requires
/// them to agree. It also means the interaction counts reported here can be
/// required to equal the CPU solver's exactly, which they are, and that is a far
/// stronger statement about the device walk than any tolerance on an
/// acceleration: it says the device visited the same cells and summed the same
/// pairs, not merely that it arrived somewhere nearby.
///
/// ## Where the tree comes from
///
/// The host builds it, exactly as `solvers/barnes_hut_solver.hpp` does and with
/// the same code. ADR-0028 records why construction stays on the CPU and only
/// the traversal moves, the short version being that the traversal is where the
/// time is and construction on this hardware is not the bottleneck it is on a
/// discrete part.
///
/// The staging that ADR-0027 introduced for Phase 9 mostly disappears here, and
/// that is a genuine architectural dividend rather than an optimisation. A tree
/// solver already has to copy the particles into the tree's sorted order before
/// it can walk anything, and already has to scatter the accelerations back to
/// the caller's order afterwards. This solver performs that gather straight into
/// shared memory and that scatter straight out of it, so the copy the GPU needs
/// is the copy the algorithm needed anyway. What remains is the node array,
/// which is O(N / leaf capacity) rather than O(N) and is measured beside
/// everything else.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/sycl_device.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"
#include "orrery/solvers/octree.hpp"

#ifdef ORRERY_ENABLE_SYCL

namespace orrery::solvers {

/// Which of the two traversals the device runs.
///
/// Both are kept and both are correct. The independent walk is not dead code
/// left behind: it is the baseline the coherent walk is measured against, and a
/// mitigation whose baseline has been deleted is a mitigation nobody can check.
/// Section 7 of the implementation plan asks for the divergence mitigation to be
/// measured rather than assumed, and this enumeration is what makes the
/// measurement a matter of running the same benchmark twice.
enum class TreeTraversal : std::uint8_t {
    /// One work-item per target, each following its own node index.
    ///
    /// The port a first attempt writes: the CPU walk with the loop over targets
    /// replaced by a work-item. Correct, and slower than it looks, for the
    /// reason this file opens with.
    kIndependent,

    /// One node index per sub-group, advanced by agreement.
    kCoherent,
};

[[nodiscard]] constexpr std::string_view to_string(TreeTraversal traversal) noexcept {
    return traversal == TreeTraversal::kCoherent ? "coherent" : "independent";
}

/// Where one force evaluation spent its time.
///
/// Six parts rather than the four `EvaluationTimings` reports, because two of
/// the four now do double duty and hiding that would make the GPU solver look
/// as though it pays a staging cost the CPU solver does not. It does not: the
/// gather and the scatter below are the same gather and scatter the CPU tree
/// solver performs, with shared memory at one end.
///
/// Taken unconditionally, for the reason `EvaluationTimings` gives: a build
/// option that switched the instrumentation off would mean the configuration
/// that reports the breakdown is not the configuration that was measured.
struct SyclTreeTimings {
    /// Computing and sorting the Morton codes, on the host.
    backend::Duration ordering{};

    /// Gathering the positions and masses into the tree's order, writing
    /// straight into shared memory. This is Phase 8's gather and Phase 9's
    /// staging at once.
    backend::Duration gathering{};

    /// Building the octree over the sorted order, on the host.
    backend::Duration construction{};

    /// Converting the host node array into the device layout.
    ///
    /// The one cost this solver carries that neither of its parents does. It is
    /// O(number of nodes), which is a fraction of N, and it is reported
    /// separately so that the fraction can be read rather than assumed.
    backend::Duration node_staging{};

    /// Submitting the traversal and waiting for the device. The figure a GPU
    /// throughput number should be computed from.
    backend::Duration kernel{};

    /// Scattering the accelerations back to the caller's order out of shared
    /// memory. Phase 8's scatter and Phase 9's copy-out at once.
    backend::Duration scatter{};
};

/// Barnes-Hut with the traversal on a SYCL device.
///
/// Declared only where the backend is compiled, on the same terms as
/// `SyclDirectSolver`. A caller that must work either way asks
/// `backend::kSyclBackendCompiled` first.
class SyclTreeSolver final : public ForceSolver {
public:
    /// A solver on this machine's GPU, or nothing.
    ///
    /// Returns null on the same three conditions `SyclDirectSolver::try_create`
    /// documents: no GPU, a device that cannot run this build's precision, or a
    /// runtime that fails to make a queue. None is an error and all three mean
    /// the caller should use the CPU tree solver, which computes the same thing.
    ///
    /// `executor` schedules the host half, which is the sort and the tree build,
    /// and is referred to rather than owned. Null runs the host half on the
    /// calling thread. The result does not depend on which, since the ordering
    /// is total and the tree is a function of the sorted particles alone.
    [[nodiscard]] static std::unique_ptr<SyclTreeSolver>
    try_create(TreeParameters parameters = {}, core::Softening softening = {},
               backend::Executor* executor = nullptr);

    ~SyclTreeSolver() override;

    SyclTreeSolver(const SyclTreeSolver&) = delete;
    SyclTreeSolver& operator=(const SyclTreeSolver&) = delete;
    SyclTreeSolver(SyclTreeSolver&&) noexcept;
    SyclTreeSolver& operator=(SyclTreeSolver&&) noexcept;

    /// Write the acceleration at each position into `accelerations`.
    ///
    /// The three views must describe the same particles in the same order and
    /// have the same length. Safe to call with no particles.
    ///
    /// Blocks until the device has finished, for the reason
    /// `SyclDirectSolver::evaluate` gives: there is nothing for the host to
    /// overlap the wait with, since the integrator cannot advance until the
    /// accelerations exist.
    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "sycl-tree"; }

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
    /// test can assert that the GPU and CPU solvers built the same tree.
    [[nodiscard]] const Octree& tree() const noexcept;

    /// Which traversal the next evaluation will run.
    [[nodiscard]] TreeTraversal traversal() const noexcept;

    void select_traversal(TreeTraversal traversal) noexcept;

    /// Ask for a particular sub-group width, and settle for the device's own
    /// choice if it cannot provide that one.
    ///
    /// Zero means the compiler chooses, which is the default and what a run
    /// should use. The knob exists because the width is the granularity of the
    /// coherence: it decides how many targets agree to walk together, and so
    /// sets both the redundancy the scheme costs and the divergence it removes.
    /// Sweeping it is the most direct measurement of the mitigation available,
    /// and `docs/performance/sycl_tree.md` reports the sweep.
    ///
    /// Only widths the device reports are accepted, so a request the hardware
    /// cannot meet becomes the default rather than a submission that throws.
    /// `supported_sub_group_widths` says which those are.
    void select_sub_group_width(unsigned width) noexcept;

    /// The width asked for, which is zero when the compiler is choosing.
    [[nodiscard]] unsigned sub_group_width() const noexcept;

    /// The widths this device will compile a kernel for, ascending.
    [[nodiscard]] std::span<const unsigned> supported_sub_group_widths() const noexcept;

    /// The work-group size the traversal launches with.
    ///
    /// A multiple of every sub-group width the device offers, since a work-group
    /// that is not one cannot be divided into whole sub-groups.
    [[nodiscard]] core::Index work_group_size() const noexcept;

    /// How many nodes the traversal visited, summed over work-items, since the
    /// count was last reset.
    ///
    /// The measurement the phase turns on, and it means different things for the
    /// two traversals, which is the point of it. For the independent walk it is
    /// the sum of the lengths of the individual walks, which is the algorithm's
    /// own figure. For the coherent walk it is the sub-group's union walk
    /// counted once per lane, so the ratio between the two is exactly the
    /// redundancy coherence costs.
    ///
    /// Reset by `reset_interaction_count`, alongside the interaction counters it
    /// belongs beside.
    [[nodiscard]] std::uint64_t node_visits() const noexcept;

    /// What the runtime says about the device this solver runs on.
    [[nodiscard]] const backend::DeviceDescription& device() const noexcept;

    /// Where the last evaluation spent its time.
    [[nodiscard]] const SyclTreeTimings& timings() const noexcept;

    /// Whether the arrays the traversal reads are shared unified memory, asked
    /// of the runtime rather than assumed.
    ///
    /// The same evidence `SyclDirectSolver::uses_shared_memory` provides, asked
    /// of the node array as well as of the particles, because the node array is
    /// the allocation this phase adds.
    [[nodiscard]] bool uses_shared_memory() const noexcept;

private:
    /// SYCL's queue and USM handles, kept out of the public interface for the
    /// reason `SyclDirectSolver` gives: otherwise `<sycl/sycl.hpp>` becomes an
    /// include of every file that mentions a solver.
    struct Impl;

    explicit SyclTreeSolver(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_SYCL
