#include "orrery/solvers/direct_solver.hpp"

#include <cstdint>
#include <span>

#include "orrery/backend/executor.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Vec3;
using core::Vec3Span;

void DirectSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                            Vec3Span<Real> accelerations) {
    const Index count = positions.size();

    // Resolved once per force evaluation and read from a local afterwards, so
    // the cost of choosing an instruction set is paid once against N^2
    // interactions. `kernel_` was already reduced to something this machine can
    // run when it was set, so this is a lookup rather than a decision.
    const AccumulateRange accumulate = accumulate_range_for(kernel_);

    // The whole kernel, over whichever targets the caller asks for. Written as
    // one callable rather than as a loop so that the threaded and unthreaded
    // paths below run the same instructions rather than two copies of them that
    // could drift apart.
    //
    // Nothing in here refers to the range it was given except as the bounds of
    // the outer loop. Each target reads every position and mass and writes only
    // its own acceleration, so any division of `[0, count)` produces the same
    // answers, in the same summation order, as any other.
    auto compute_targets = [&](Index begin, Index end) {
        for (Index i = begin; i < end; ++i) {
            const Vec3 target = positions.get(i);

            // Every particle before this one, then every particle after it. The
            // arithmetic is written in the order the particles are stored rather
            // than in the order that would let the two sums be combined, because
            // a reference implementation whose summation order depends on the
            // index it is called with is a poor thing to compare a faster kernel
            // against.
            const Vec3 before = accumulate(positions, masses, target, 0, i, softening_);
            const Vec3 after = accumulate(positions, masses, target, i + 1, count, softening_);

            // G is one in this project's units (ADR-0007), so this
            // multiplication costs nothing at run time. It is written because a
            // gravitational acceleration with no G in it reads as though the
            // physics had been forgotten rather than as a choice of units.
            accelerations.set(i, core::kGravitationalConstant * (before + after));
        }
    };

    if (executor_ == nullptr) {
        compute_targets(0, count);
    } else {
        executor_->run(count, compute_targets);
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
