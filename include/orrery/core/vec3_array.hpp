#pragma once

/// \file
/// Storage for a vector quantity that is not part of the particle state.
///
/// `ParticleData` holds the ten arrays a particle is made of, and its invariant
/// is that all ten agree in length. That is exactly what a simulation wants and
/// exactly what an intermediate result does not: a Runge-Kutta stage needs a
/// saved copy of the positions and a running sum of stage derivatives, and
/// neither has a mass or an acceleration to go with it. Allocating a whole
/// particle store to use three of its arrays would move a third more memory than
/// the work requires and would invite a reader to ask which of the unused seven
/// arrays were meaningful.
///
/// So this is the same layout decision as ADR-0004 applied to one quantity
/// rather than to ten: three cache-line-aligned component arrays, viewed through
/// the same `Vec3Span` that every kernel already takes. It lives in `core`
/// because the layout and the allocator are core's decisions, not because any
/// part of `core` uses it yet.

#include <vector>

#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::core {

/// A resizable array of 3-vectors in component-array layout.
///
/// The three components are always the same length, for the same reason and by
/// the same means as in `ParticleData`: every operation that can change a length
/// applies to all three.
class Vec3Array {
public:
    Vec3Array() = default;

    /// Create `count` vectors, every component zero.
    explicit Vec3Array(Index count);

    [[nodiscard]] Index size() const noexcept { return x_.size(); }

    [[nodiscard]] bool empty() const noexcept { return x_.empty(); }

    /// Change the length, zero-filling anything new.
    ///
    /// The intended use is a scratch buffer that a repeated operation resizes on
    /// its first call and finds already the right length on every call after,
    /// which is what keeps an integrator step free of allocation.
    void resize(Index count);

    [[nodiscard]] Vec3Span<Real> view() noexcept { return {x_, y_, z_}; }

    [[nodiscard]] Vec3Span<const Real> view() const noexcept { return {x_, y_, z_}; }

private:
    using ComponentArray = std::vector<Real, AlignedAllocator<Real>>;

    ComponentArray x_;
    ComponentArray y_;
    ComponentArray z_;
};

} // namespace orrery::core
