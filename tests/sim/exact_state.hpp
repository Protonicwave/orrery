#pragma once

/// \file
/// Exact comparison of two particle states.
///
/// Every other comparison in this project's test suite is against a tolerance,
/// because it is comparing a computed answer with an analytic one. This one is
/// not, and the difference is the point: section 7 of the implementation plan
/// requires an interrupted run to resume to bitwise-identical state, so the test
/// that demonstrates it has to compare bits. A tolerance here would pass for a
/// resume that had lost the last few digits of every velocity, which is exactly
/// the defect the requirement exists to exclude.

#include <span>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::sim::testing {

[[nodiscard]] inline bool identical(std::span<const core::Real> left,
                                    std::span<const core::Real> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (core::Index index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool identical(core::Vec3Span<const core::Real> left,
                                    core::Vec3Span<const core::Real> right) noexcept {
    return identical(left.x, right.x) && identical(left.y, right.y) && identical(left.z, right.z);
}

/// Whether two states agree in every bit of all ten component arrays.
[[nodiscard]] inline bool identical_states(const core::ParticleData& left,
                                           const core::ParticleData& right) noexcept {
    return left.size() == right.size() && identical(left.masses(), right.masses()) &&
           identical(left.positions(), right.positions()) &&
           identical(left.velocities(), right.velocities()) &&
           identical(left.accelerations(), right.accelerations());
}

} // namespace orrery::sim::testing
