#pragma once

/// \file
/// Three parallel component arrays, viewed as one sequence of 3-vectors.
///
/// The storage layout keeps the x, y and z components of a vector quantity in
/// separate arrays (ADR-0004), which is what the force kernel wants and what
/// everything else finds awkward. This view is the bridge. It carries the three
/// spans together, so that a function taking a vector quantity takes one
/// argument whose parts cannot be mismatched, and it offers element access in
/// terms of `Vec3` for the code that thinks in vectors rather than in
/// components.
///
/// Kernels use the spans directly and ignore the element access, which is the
/// intended division: `get` and `set` are a gather and a scatter, and doing
/// either one per particle inside a loop would defeat the layout they are built
/// on.

#include <span>
#include <type_traits>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::core {

/// A view of three component arrays of equal length.
///
/// `T` is `Real` for a mutable view and `const Real` for a read-only one. Like
/// `std::span`, this is a handle rather than a container: copying it is cheap,
/// it does not own what it points at, and its own constness says nothing about
/// whether the elements can be written.
template<typename T> struct Vec3Span {
    static_assert(std::is_same_v<std::remove_const_t<T>, Real>,
                  "A Vec3Span views component arrays, which hold Real");

    std::span<T> x;
    std::span<T> y;
    std::span<T> z;

    /// The number of vectors in the view.
    ///
    /// Reported from one component because the three are always the same
    /// length. Every view of this kind comes from `ParticleData`, which grows
    /// its arrays together and has no path that could leave them differing.
    [[nodiscard]] constexpr Index size() const noexcept { return x.size(); }

    [[nodiscard]] constexpr bool empty() const noexcept { return x.empty(); }

    /// Gather one vector from the three arrays.
    [[nodiscard]] constexpr Vec3 get(Index index) const noexcept {
        return {x[index], y[index], z[index]};
    }

    /// Scatter one vector across the three arrays.
    ///
    /// A const member for the same reason `std::span::operator[]` is one: what
    /// is const here is the handle, not the elements it refers to.
    constexpr void set(Index index, Vec3 value) const noexcept
        requires(!std::is_const_v<T>)
    {
        x[index] = value.x;
        y[index] = value.y;
        z[index] = value.z;
    }

    /// A mutable view converts to a read-only one, so that a caller holding
    /// write access can pass it to something that only reads.
    constexpr operator Vec3Span<const T>() const noexcept
        requires(!std::is_const_v<T>)
    {
        return {x, y, z};
    }
};

} // namespace orrery::core
