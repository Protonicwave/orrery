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
/// later phase. This implementation is single-threaded and scalar: threading
/// arrives in Phase 6 and explicit vectorisation in Phase 7, both as work on this
/// kernel rather than as second copies of it. What is written here is the
/// arithmetic, in the layout the later phases need, and nothing else.

#include <span>
#include <string_view>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"

namespace orrery::solvers {

/// The O(N^2) solver described above.
class DirectSolver final : public ForceSolver {
public:
    /// A solver over exact point masses.
    DirectSolver() = default;

    /// A solver softening at the given length.
    ///
    /// `explicit` for the reason `Softening`'s own constructor is: the argument
    /// is the modelling decision, not a spelling of the solver.
    explicit DirectSolver(core::Softening softening) noexcept : softening_(softening) {}

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

private:
    core::Softening softening_;
    InteractionCount count_;
};

} // namespace orrery::solvers
