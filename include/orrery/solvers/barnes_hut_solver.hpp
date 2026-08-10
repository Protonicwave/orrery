#pragma once

/// \file
/// The hierarchical solver: the same physics as direct summation, with most of
/// the interactions replaced by an approximation whose error is bounded and
/// measured.
///
/// Direct summation computes N(N-1) interactions and is exact. This solver
/// groups distant particles into cells and computes one interaction per cell,
/// which turns the cost into something close to N log N and introduces an error
/// controlled by one parameter. That trade is the main algorithmic contribution
/// of this project, and the point of having kept the direct solver is that the
/// error is measured against it rather than argued about.
///
/// ## What actually happens in one evaluation
///
/// Four steps, in order, and the first three exist to make the fourth fast.
///
/// The particles are sorted along a space-filling curve
/// (`solvers/morton.hpp`), which puts neighbours in space next to each other in
/// memory. An octree is built over the sorted order, which is cheap precisely
/// because the sort already put every cell's particles in one contiguous range
/// (`solvers/octree.hpp`). The positions and masses are gathered into that
/// order, so that the traversal reads them sequentially rather than following
/// the permutation on every access. Then every particle walks the tree
/// (`solvers/tree_walk.hpp`), and the accelerations are written back to the
/// caller's order on the way out.
///
/// All four are redone from scratch on every force evaluation. The positions
/// have moved by the time the next one is asked for, and a tree that was
/// updated rather than rebuilt would be a tree whose accuracy depended on how
/// long ago it was built. ADR-0022 records what that costs, which is measured
/// in `docs/performance/barnes_hut.md` and is a small fraction of the walk.
///
/// ## The accuracy knobs
///
/// Two, and they are not equivalent. The opening angle decides how far away a
/// cell has to be before it stands in for its contents, and lowering it buys
/// accuracy by computing more interactions. The quadrupole moment adds the next
/// term of the expansion to every cell that is accepted, and buys accuracy by
/// computing a more expensive interaction rather than more of them. Which is
/// the better way to buy a given error is a question about the machine, and
/// `docs/performance/barnes_hut.md` answers it by measuring both.
///
/// ## What this solver does not conserve
///
/// Momentum, exactly. Direct summation in the form ADR-0015 chose computes each
/// pair from both ends, and the two halves cancel to the last bit because they
/// are the same magnitude computed from the same numbers. A tree evaluation has
/// no such symmetry: particle i may see particle j through a cell while j sees
/// i directly, so the two forces are not equal and opposite, and the total
/// momentum of a configuration drifts.
///
/// This is a property of the method rather than a defect of the implementation,
/// it is the reason the direct solver remains the reference, and it is bounded
/// by the same opening angle that bounds everything else.
/// `tests/solvers/barnes_hut_test.cpp` measures the violation and asserts it
/// stays at the size the approximation implies rather than asserting it is
/// zero, which would be asserting something false.

#include <span>
#include <string_view>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"
#include "orrery/solvers/morton.hpp"
#include "orrery/solvers/octree.hpp"

namespace orrery::solvers {

/// Where the time of one force evaluation went.
///
/// Three clock readings per evaluation, against a walk measured in
/// milliseconds. They are taken unconditionally because the alternative, a
/// build option or a template parameter, would mean the configuration that
/// reports the breakdown is not the configuration that was measured.
///
/// This is not a substitute for the harness in `benchmarks/`. It is a division
/// of one evaluation into its parts, and a benchmark still has to run many of
/// them and report a median with its dispersion. What it answers is the
/// question a single total cannot: whether a tree solver that is slower than
/// expected is slow at building or slow at walking, which are different
/// problems with different remedies.
struct EvaluationTimings {
    /// Computing and sorting the Morton codes.
    backend::Duration ordering{};

    /// Building the octree over the sorted order, moments included.
    backend::Duration construction{};

    /// Gathering the sorted positions and masses.
    backend::Duration gathering{};

    /// Walking the tree for every particle, which is the part that should
    /// dominate.
    backend::Duration traversal{};
};

/// The Barnes-Hut solver described above.
class BarnesHutSolver final : public ForceSolver {
public:
    /// A solver over exact point masses, evaluated on the calling thread.
    ///
    /// The parameters are corrected on the way in, so a solver constructed with
    /// an opening angle of two reports one and behaves accordingly.
    explicit BarnesHutSolver(TreeParameters parameters = {}) noexcept
        : parameters_(corrected_parameters(parameters)) {}

    /// A solver softening at the given length.
    BarnesHutSolver(TreeParameters parameters, core::Softening softening) noexcept
        : parameters_(corrected_parameters(parameters)), softening_(softening) {}

    /// A softened solver evaluated through `executor`.
    ///
    /// The executor is referred to rather than owned and must outlive the
    /// solver, for the reason `DirectSolver` gives: a pool of threads is a
    /// machine-wide resource and a solver that owned one could not be created
    /// inside a loop.
    BarnesHutSolver(TreeParameters parameters, core::Softening softening,
                    backend::Executor& executor) noexcept
        : parameters_(corrected_parameters(parameters)),
          softening_(softening),
          executor_(&executor) {}

    /// Write the acceleration at each position into `accelerations`.
    ///
    /// The three views must describe the same particles in the same order and
    /// have the same length, as `AccelerationField` requires.
    ///
    /// Safe to call with no particles.
    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "barnes-hut"; }

    [[nodiscard]] core::Softening softening() const noexcept override { return softening_; }

    [[nodiscard]] InteractionCount interaction_count() const noexcept override { return count_; }

    void reset_interaction_count() noexcept override { count_ = {}; }

    /// The parameters in force, which are the ones asked for after correction.
    [[nodiscard]] const TreeParameters& parameters() const noexcept { return parameters_; }

    /// The tree the last evaluation built, empty before the first.
    ///
    /// Exposed so that a test can assert what the tree is and a benchmark can
    /// report its shape beside its timing. A depth or a leaf count is the first
    /// thing to look at when a tree costs more than it should, and recovering
    /// either from outside would mean building a second tree.
    [[nodiscard]] const Octree& tree() const noexcept { return tree_; }

    /// Where the last evaluation's time went.
    [[nodiscard]] const EvaluationTimings& timings() const noexcept { return timings_; }

    /// The executor this solver evaluates through, or null if it runs on the
    /// calling thread.
    [[nodiscard]] const backend::Executor* executor() const noexcept { return executor_; }

    /// Ask for a particular kernel for the pairs at the leaves, and settle for
    /// the scalar one if this machine cannot run it.
    ///
    /// The same knob `DirectSolver` offers and for the same callers: the
    /// accuracy test comparing kernels and the benchmark timing them. It
    /// matters more here than it looks. A tree solver's leaves are short
    /// ranges, so the vector kernel is called with a few dozen sources rather
    /// than a few thousand, and how much of Phase 7's speedup survives that is
    /// a measurement rather than a deduction.
    void select_kernel(KernelKind kind) noexcept {
        kernel_ = kernel_available(kind) ? kind : KernelKind::kScalar;
    }

    /// The kernel this solver will use at the leaves.
    [[nodiscard]] KernelKind kernel() const noexcept { return kernel_; }

private:
    using ComponentArray = std::vector<core::Real, core::AlignedAllocator<core::Real>>;

    /// Copy the positions and masses into the sorted order.
    ///
    /// The gather is the price of not owning the particle store. A solver that
    /// could permute the caller's arrays would not need it, and ADR-0021
    /// records why this project would rather pay four sequential writes and
    /// four scattered reads per particle than let a force solver reorder a
    /// configuration underneath the integrator that owns it.
    void gather(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses);

    TreeParameters parameters_;
    core::Softening softening_;

    /// Not owned, and null by default, exactly as on `DirectSolver`.
    backend::Executor* executor_{nullptr};

    KernelKind kernel_{fastest_available_kernel()};

    MortonOrdering ordering_;
    Octree tree_;

    /// The configuration in the tree's order.
    ///
    /// Held rather than recomputed, so that the arrays are allocated on the
    /// first evaluation and reused by every later one. Component arrays rather
    /// than an array of positions, because the direct kernel that sums the
    /// leaves reads them exactly as it reads the caller's (ADR-0004), and the
    /// aligned allocator because the vector kernel loads from them.
    ComponentArray sorted_x_;
    ComponentArray sorted_y_;
    ComponentArray sorted_z_;
    ComponentArray sorted_mass_;

    InteractionCount count_;
    EvaluationTimings timings_;
};

} // namespace orrery::solvers
