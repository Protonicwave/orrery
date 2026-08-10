#include "orrery/solvers/reference_kernel.hpp"

#include <cmath>
#include <span>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

namespace {

using core::Index;
using core::Real;
using core::Softening;
using core::Vec3Span;

/// A running sum that keeps the bits ordinary addition throws away.
///
/// Neumaier's variant of compensated summation rather than Kahan's. The two
/// differ in one case and it is the case that matters here: when the term being
/// added is larger in magnitude than the running total, Kahan's formulation
/// loses the compensation and Neumaier's does not. A force sum runs over
/// particles at every distance, so a term much larger than the partial sum so
/// far is ordinary rather than exotic, and it is the first term of any sum.
///
/// The correction is added once at the end rather than folded back on each
/// step. Folding it back would round it into the total and defeat the purpose.
class CompensatedSum {
public:
    void add(double term) noexcept {
        const double updated = total_ + term;

        // Recover the low-order bits the addition above discarded. Which of the
        // two operands to subtract from depends on which is larger, and getting
        // that test the wrong way round is the whole difference between this
        // and the naive version.
        if (std::abs(total_) >= std::abs(term)) {
            correction_ += (total_ - updated) + term;
        } else {
            correction_ += (term - updated) + total_;
        }

        total_ = updated;
    }

    [[nodiscard]] double value() const noexcept { return total_ + correction_; }

private:
    double total_{};
    double correction_{};
};

} // namespace

ReferenceAcceleration reference_acceleration(Vec3Span<const Real> positions,
                                             std::span<const Real> masses, Index target,
                                             Softening softening) {
    const auto target_x = static_cast<double>(positions.x[target]);
    const auto target_y = static_cast<double>(positions.y[target]);
    const auto target_z = static_cast<double>(positions.z[target]);
    const auto softening_squared = static_cast<double>(softening.squared());

    CompensatedSum acceleration_x;
    CompensatedSum acceleration_y;
    CompensatedSum acceleration_z;

    const Index count = positions.size();

    for (Index j = 0; j < count; ++j) {
        // The branch the kernels go to some trouble to avoid, which costs
        // nothing here and makes the exclusion of the self term impossible to
        // misread. This function is an instrument, not a kernel.
        if (j == target) {
            continue;
        }

        const double dx = static_cast<double>(positions.x[j]) - target_x;
        const double dy = static_cast<double>(positions.y[j]) - target_y;
        const double dz = static_cast<double>(positions.z[j]) - target_z;

        const double separation_squared = (dx * dx) + (dy * dy) + (dz * dz) + softening_squared;

        // Written out rather than taken from `core/softening.hpp`, which works
        // in `Real` and would round every term back to the precision under
        // test in the single-precision build. The expression is the same one:
        // the reciprocal of the softened distance, cubed.
        const double inverse = 1.0 / std::sqrt(separation_squared);
        const double factor = static_cast<double>(masses[j]) * inverse * inverse * inverse;

        acceleration_x.add(factor * dx);
        acceleration_y.add(factor * dy);
        acceleration_z.add(factor * dz);
    }

    // G is one (ADR-0007), and is written for the reason the solver writes it.
    const auto gravitational_constant = static_cast<double>(core::kGravitationalConstant);

    return {gravitational_constant * acceleration_x.value(),
            gravitational_constant * acceleration_y.value(),
            gravitational_constant * acceleration_z.value()};
}

} // namespace orrery::solvers
