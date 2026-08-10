#include <span>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Softening;
using core::Vec3;
using core::Vec3Span;

// The reference kernel, unchanged from Phase 5 except for having moved into a
// file of its own. It is deliberately the plainest expression of the sum in the
// project: one pair per iteration, in index order, in the order the terms are
// written down in the header of `direct_solver.hpp`.
//
// It is not tuned and should not be. Its job is to be the answer everything
// else is compared against, and every optimisation applied to it is a
// difference that would then have to be argued about when a faster kernel
// disagrees with it.
//
// The components are read straight from the three arrays rather than gathered
// into a `Vec3` per pair. That is the layout decision of ADR-0004 being spent:
// each of the four reads below walks its own contiguous array in step with the
// others.
Vec3 accumulate_range_scalar(Vec3Span<const Real> positions, std::span<const Real> masses,
                             Vec3 target, Index begin, Index end, Softening softening) noexcept {
    Real acceleration_x = 0;
    Real acceleration_y = 0;
    Real acceleration_z = 0;

    for (Index j = begin; j < end; ++j) {
        const Real dx = positions.x[j] - target.x;
        const Real dy = positions.y[j] - target.y;
        const Real dz = positions.z[j] - target.z;

        // The squared separation, never the separation. A square root here
        // would be the most expensive operation in the loop, and the
        // inverse-square law wants the square back immediately afterwards.
        const Real separation_squared = (dx * dx) + (dy * dy) + (dz * dz);

        // Mass and geometry combined into one factor before either touches the
        // three components, so the multiplication by the separation vector is
        // three operations rather than six.
        const Real factor =
            masses[j] * core::softened_inverse_distance_cubed(separation_squared, softening);

        acceleration_x += factor * dx;
        acceleration_y += factor * dy;
        acceleration_z += factor * dz;
    }

    return {acceleration_x, acceleration_y, acceleration_z};
}

} // namespace orrery::solvers
