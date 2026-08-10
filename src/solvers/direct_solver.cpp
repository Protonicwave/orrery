#include "orrery/solvers/direct_solver.hpp"

#include <cstdint>
#include <span>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

namespace {

using core::Index;
using core::Real;
using core::Softening;
using core::Vec3;
using core::Vec3Span;

/// The acceleration on a particle at `target` from the particles in
/// `[begin, end)`, without the factor of G.
///
/// The range is a parameter because the one particle a body does not attract is
/// itself, and the two ranges either side of it are how that is arranged. The
/// obvious alternative, one loop over every particle with `if (i == j) continue`
/// inside it, puts a branch in the innermost loop of the whole project to skip
/// one iteration in N. Worse, it hides a real question behind a test that looks
/// like bookkeeping: unsoftened, the self term is a zero separation over a zero
/// distance, and that is a division of zero by zero producing a NaN that would
/// spread to every particle. Two ranges that exclude the index cannot be got
/// wrong by an optimiser reordering the comparison, and they leave two loops with
/// no control flow in them for Phase 7 to vectorise.
///
/// The components are read straight from the three arrays rather than gathered
/// into a `Vec3` per pair. That is the layout decision of ADR-0004 being spent:
/// each of the four reads below walks its own contiguous array in step with the
/// others.
[[nodiscard]] Vec3 accumulate_range(Vec3Span<const Real> positions, std::span<const Real> masses,
                                    Vec3 target, Index begin, Index end,
                                    Softening softening) noexcept {
    Real acceleration_x = 0;
    Real acceleration_y = 0;
    Real acceleration_z = 0;

    for (Index j = begin; j < end; ++j) {
        const Real dx = positions.x[j] - target.x;
        const Real dy = positions.y[j] - target.y;
        const Real dz = positions.z[j] - target.z;

        // The squared separation, never the separation. A square root here would
        // be the most expensive operation in the loop, and the inverse-square law
        // wants the square back immediately afterwards.
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

} // namespace

void DirectSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                            Vec3Span<Real> accelerations) {
    const Index count = positions.size();

    for (Index i = 0; i < count; ++i) {
        const Vec3 target = positions.get(i);

        // Every particle before this one, then every particle after it. The
        // arithmetic is written in the order the particles are stored rather
        // than in the order that would let the two sums be combined, because a
        // reference implementation whose summation order depends on the index it
        // is called with is a poor thing to compare a faster kernel against.
        const Vec3 before = accumulate_range(positions, masses, target, 0, i, softening_);
        const Vec3 after = accumulate_range(positions, masses, target, i + 1, count, softening_);

        // G is one in this project's units (ADR-0007), so this multiplication
        // costs nothing at run time. It is written because a gravitational
        // acceleration with no G in it reads as though the physics had been
        // forgotten rather than as a choice of units.
        accelerations.set(i, core::kGravitationalConstant * (before + after));
    }

    ++count_.evaluations;

    // In closed form, once, rather than incremented inside the loop. A counter
    // in the innermost loop would be a dependency between iterations that
    // prevents vectorisation, and it would make the measurement change the thing
    // it measures. Direct summation is the one algorithm whose work is known
    // exactly before it starts, and this is the whole of the benefit of that.
    //
    // Widened before multiplying, not after: on a 32-bit platform `Index` is 32
    // bits, and the product exceeds it above about 65,000 particles.
    if (count > 1) {
        const auto pairs = static_cast<std::uint64_t>(count);
        count_.particle_particle += pairs * (pairs - 1);
    }
}

} // namespace orrery::solvers
