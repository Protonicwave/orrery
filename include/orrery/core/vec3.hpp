#pragma once

/// \file
/// A three-component vector for interfaces, not for storage.
///
/// Particles are stored as separate component arrays rather than as vectors
/// (ADR-0004), so this type never appears in the inner loop of a force kernel.
/// It exists so that setup code, diagnostics and tests can pass a position or a
/// velocity as one value rather than as three loose scalars, which is where
/// transposed arguments come from. Every operation is `constexpr` so that an
/// analytic configuration, a Kepler orbit among them, can be written down at
/// compile time and checked by the compiler rather than at run time.

#include <cmath>

#include "orrery/core/types.hpp"

namespace orrery::core {

/// A vector in three-dimensional Euclidean space.
///
/// An aggregate with public members, because it is data. Wrapping three
/// coordinates in accessors would buy no invariant, since every combination of
/// three finite numbers is a valid vector, and would obstruct the aggregate
/// initialisation that makes analytic test configurations readable.
struct Vec3 {
    Real x{};
    Real y{};
    Real z{};

    constexpr Vec3& operator+=(Vec3 other) noexcept {
        x += other.x;
        y += other.y;
        z += other.z;
        return *this;
    }

    constexpr Vec3& operator-=(Vec3 other) noexcept {
        x -= other.x;
        y -= other.y;
        z -= other.z;
        return *this;
    }

    constexpr Vec3& operator*=(Real scale) noexcept {
        x *= scale;
        y *= scale;
        z *= scale;
        return *this;
    }

    constexpr Vec3& operator/=(Real divisor) noexcept {
        x /= divisor;
        y /= divisor;
        z /= divisor;
        return *this;
    }

    /// Exact component-wise equality.
    ///
    /// This answers whether two vectors are the same value, which is the
    /// question storage round-trip tests ask. It is not the question physics
    /// asks: a comparison between a computed result and an expected one is a
    /// comparison against a tolerance, stated at the point of comparison where
    /// the reader can see what accuracy is being claimed.
    [[nodiscard]] constexpr bool operator==(const Vec3& other) const = default;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 v) noexcept {
    return {-v.x, -v.y, -v.z};
}

[[nodiscard]] constexpr Vec3 operator*(Vec3 v, Real scale) noexcept {
    return {v.x * scale, v.y * scale, v.z * scale};
}

[[nodiscard]] constexpr Vec3 operator*(Real scale, Vec3 v) noexcept {
    return v * scale;
}

[[nodiscard]] constexpr Vec3 operator/(Vec3 v, Real divisor) noexcept {
    return {v.x / divisor, v.y / divisor, v.z / divisor};
}

[[nodiscard]] constexpr Real dot(Vec3 a, Vec3 b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

/// The squared length.
///
/// Named separately rather than left as `dot(v, v)` because the squared length
/// is what the physics actually wants nearly everywhere. A gravitational force
/// falls as the inverse square of the separation, so a kernel that took a
/// square root only to square it again would pay for the most expensive
/// operation in the loop and then discard it.
[[nodiscard]] constexpr Real squared_norm(Vec3 v) noexcept {
    return dot(v, v);
}

/// The length.
///
/// Not `constexpr`: `std::sqrt` only becomes usable in constant expressions in
/// C++26, and this project is C++20. The compile-time configurations that need
/// a magnitude can use `squared_norm` instead.
[[nodiscard]] inline Real norm(Vec3 v) noexcept {
    return std::sqrt(squared_norm(v));
}

} // namespace orrery::core
