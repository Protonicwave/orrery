#pragma once

/// \file
/// Direct summation: every particle against every other, and the standard the
/// rest of the project is measured against.
///
/// The acceleration of particle i is the sum over all other particles of
///
///     a_i = G sum_j m_j (r_j - r_i) / (|r_j - r_i|^2 + eps^2)^(3/2)
///
/// which is the gradient of the softened potential in `core/softening.hpp`, and
/// nothing else. There is no approximation in it, no expansion truncated at some
/// order and no cell opened or left closed. Its only error is floating-point
/// round-off, and in double precision that is smaller than every other error in
/// the project by many orders of magnitude.
///
/// That is what makes it the reference. Every claim Orrery goes on to make about
/// a faster method, a tree opened at some angle, a quadrupole moment included or
/// dropped, a kernel in single precision on the GPU, is a claim about the
/// difference between that method's answer and this one's. The direct solver is
/// therefore not deleted once faster methods exist, and it is not optimised to
/// the point where its correctness stops being obvious.
///
/// ## Softening
///
/// The kernel softens with the Plummer form, whose derivation and physical
/// justification are set out in `core/softening.hpp`: it is the exact potential
/// of a Plummer sphere of scale radius `eps`, so a softened run is an exact
/// simulation of extended masses rather than an approximate one of point masses,
/// and the potential energy diagnostic differentiates the same expression this
/// kernel does.
///
/// What that file does not settle is which length to use, because the answer is
/// a property of the configuration rather than of the kernel. The trade is
/// between two errors that move in opposite directions. Too small a softening
/// leaves the pairwise force nearly singular, so a chance close approach between
/// two particles produces an acceleration no fixed timestep can follow, and the
/// simulation acquires energy from the integration rather than from the physics.
/// Too large a softening biases the force at separations that matter, suppressing
/// structure the run was meant to resolve. Published rules of thumb for the
/// optimum differ by an order of magnitude and depend on what is being measured,
/// so this project adopts none of them as a default: a solver constructed without
/// a softening applies none, and every result quotes the length it used.
///
/// Zero is the right default for the same reason the analytic configurations
/// exist. A Kepler orbit is an orbit of two point masses, and softening it would
/// replace the problem whose solution is known with a nearby problem whose
/// solution is not. A default that quietly softened would make the project's
/// primary validation instrument approximate without saying so.
///
/// ## Cost
///
/// N(N-1) interactions per evaluation, which is the cost that motivates every
/// later phase. Phase 6 divided the loop over target particles between threads
/// and Phase 7 vectorised the loop inside it, both as work on this kernel
/// rather than as second copies of it. The physics below is the physics Phase 5
/// validated; what changed is who runs which iterations of the outer loop and
/// how many pairs the inner one handles at once.
///
/// ## Vectorisation
///
/// The solver holds no arithmetic of its own any more. The summation over a
/// range of sources lives in `solvers/direct_kernel.hpp`, in a scalar version
/// and an AVX2 version, and this class chooses between them once per force
/// evaluation. That file sets out the two ways the vector kernel's answer
/// differs from the scalar one, reassociated summation and fused
/// multiply-add, and neither is a relaxation of IEEE arithmetic; ADR-0020
/// records that the project enables no fast-math flag anywhere.
///
/// The default is the fastest kernel the machine can run, which is what a
/// simulation wants. A test or a benchmark that needs a particular one asks for
/// it with `select_kernel` and reads back what it got with `kernel`, because a
/// request for a kernel the processor does not implement is answered with the
/// scalar one rather than refused.
///
/// ## Threading
///
/// The solver does not contain a thread. It is handed an executor from
/// `backend/`, which divides the range of target particles among however many
/// workers it has, and the kernel is written so that this division changes
/// nothing about the answer. Each target reads every position and mass and
/// writes only its own acceleration, so no two workers write to the same place
/// and none reads what another wrote. ADR-0015 chose that form in Phase 5 partly
/// for this reason, and ADR-0017 records the seam.
///
/// The consequence worth stating is that a threaded evaluation is bit for bit
/// identical to a serial one. Particle i's acceleration is summed in index order
/// whichever worker happens to compute it, so no result depends on the thread
/// count, the chunk size or the order the chunks were claimed in. That is what
/// makes the direct solver still usable as the project's reference after being
/// parallelised, and it is asserted by test rather than assumed.

#include <span>
#include <string_view>

#include "orrery/backend/executor.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"

namespace orrery::solvers {

/// The O(N^2) solver described above.
class DirectSolver final : public ForceSolver {
public:
    /// A solver over exact point masses, evaluated on the calling thread.
    DirectSolver() = default;

    /// A solver softening at the given length.
    ///
    /// `explicit` for the reason `Softening`'s own constructor is: the argument
    /// is the modelling decision, not a spelling of the solver.
    explicit DirectSolver(core::Softening softening) noexcept : softening_(softening) {}

    /// A solver over exact point masses, evaluated through `executor`.
    explicit DirectSolver(backend::Executor& executor) noexcept : executor_(&executor) {}

    /// A softened solver evaluated through `executor`.
    ///
    /// The executor is referred to rather than owned, and must outlive the
    /// solver. That is the right ownership for what it is: a pool of threads is
    /// a machine-wide resource, one is enough for a whole simulation, and a
    /// solver that owned one could not be created inside a loop without
    /// creating and destroying eight threads each time round.
    DirectSolver(core::Softening softening, backend::Executor& executor) noexcept
        : softening_(softening), executor_(&executor) {}

    /// Write the acceleration at each position into `accelerations`.
    ///
    /// The three views must describe the same particles in the same order and
    /// have the same length, as `AccelerationField` requires. That precondition
    /// is not checked here: the check would have to throw or terminate, and no
    /// exception may leave a kernel, so it belongs at the boundary where the
    /// spans are formed rather than in the loop that consumes them.
    ///
    /// Safe to call with no particles, which is what an empty configuration and
    /// a partially built one both look like.
    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "direct"; }

    [[nodiscard]] core::Softening softening() const noexcept override { return softening_; }

    [[nodiscard]] InteractionCount interaction_count() const noexcept override { return count_; }

    void reset_interaction_count() noexcept override { count_ = {}; }

    /// The executor this solver evaluates through, or null if it runs on the
    /// calling thread.
    ///
    /// Exposed so that a benchmark can report which scheme produced a timing
    /// alongside the timing itself, which is the one thing a threading figure
    /// is useless without.
    [[nodiscard]] const backend::Executor* executor() const noexcept { return executor_; }

    /// Ask for a particular kernel, and settle for the scalar one if this
    /// machine cannot run it.
    ///
    /// Deliberately not a constructor argument. A simulation should take the
    /// fastest kernel available and never mention the subject; the callers that
    /// care are the accuracy test comparing two kernels on one configuration
    /// and the benchmark timing them against each other, and both of those want
    /// to change the choice on a solver they already hold rather than to build
    /// a second one.
    ///
    /// Silently falling back rather than refusing is the right behaviour for
    /// the same reason `accumulate_range_for` never returns null: a request for
    /// AVX2 on a machine without it should produce correct physics, and the
    /// caller that cares which kernel ran asks `kernel()`.
    void select_kernel(KernelKind kind) noexcept {
        kernel_ = kernel_available(kind) ? kind : KernelKind::kScalar;
    }

    /// The kernel this solver will actually use.
    [[nodiscard]] KernelKind kernel() const noexcept { return kernel_; }

private:
    core::Softening softening_;

    /// Not owned, and null by default.
    ///
    /// Null means the calling thread runs the whole range, which is exactly
    /// what Phase 5 did and what most of the test suite still wants: a
    /// two-particle configuration costs less to compute than to hand to a pool.
    /// It is a plain pointer rather than a reference to a shared serial
    /// executor because such a default would be global mutable state, and two
    /// solvers on two threads accumulating into one set of counters would race
    /// over statistics neither of them asked for.
    backend::Executor* executor_{nullptr};

    /// The fastest kernel this machine can run, unless a caller says otherwise.
    ///
    /// Resolved when the solver is constructed rather than on every evaluation,
    /// because the answer cannot change while the process runs and because a
    /// feature query inside a force evaluation would be the sort of small
    /// avoidable cost this project spends comments avoiding elsewhere.
    KernelKind kernel_{fastest_available_kernel()};

    InteractionCount count_;
};

} // namespace orrery::solvers
