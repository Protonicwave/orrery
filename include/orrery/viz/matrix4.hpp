#pragma once

/// \file
/// The 4-by-4 transforms a camera needs, and nothing else.
///
/// This is not a linear algebra library. It holds the three matrices a point
/// renderer uses, a view, a projection and the product of the two, along with
/// the operations needed to build and apply them. Anything a general matrix type
/// would also offer, an inverse, a decomposition, an arbitrary-size template,
/// would be code with one caller or none.
///
/// ## Why this is float when the rest of the project may be double
///
/// Every number here ends up in a `uniform mat4` or a vertex attribute, and
/// OpenGL's fixed-function pipeline works in single precision whatever is handed
/// to it. A double-precision camera would be exact about a quantity that becomes
/// a 24-bit depth value and a pixel on a screen. The simulation's own precision
/// is a separate decision, made in `core/types.hpp` for reasons that are about
/// the physics; this one is about the display.
///
/// The conversion happens at the boundary, where a position in the simulation's
/// scalar type becomes a coordinate in the camera's. That is safe for the
/// coordinates this project produces, which are of order tens in N-body units
/// and so are represented to seven significant figures, far finer than a pixel.
///
/// ## Layout
///
/// Column-major, so `element(row, column)` is `values[column * 4 + row]`. That
/// is the layout `glUniformMatrix4fv` expects with transposition off, which
/// means a matrix built here can be handed to the driver without a copy or a
/// flag. The cost is that the initialiser lists below read as the transpose of
/// the matrix they represent, so each one is written a column at a time and
/// says so.

#include <array>
#include <cstddef>

#include "orrery/core/vec3.hpp"

namespace orrery::viz {

/// A point or direction in homogeneous coordinates.
///
/// Exists so that a transform can be applied to something and the result
/// inspected, which is what the tests do and what a projection's `w` component
/// is needed for. Rendering never constructs one: the vertex shader does that
/// work on the device.
struct Vec4 {
    float x{};
    float y{};
    float z{};
    float w{};

    [[nodiscard]] constexpr bool operator==(const Vec4&) const = default;
};

/// A 4-by-4 transform in column-major order.
struct Mat4 {
    std::array<float, 16> values{};

    [[nodiscard]] constexpr float element(std::size_t row, std::size_t column) const noexcept {
        return values[(column * 4) + row];
    }

    [[nodiscard]] constexpr bool operator==(const Mat4&) const = default;
};

[[nodiscard]] constexpr Mat4 identity() noexcept {
    return Mat4{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}

/// The transform that applies `b` and then `a`, as matrix products conventionally
/// read.
[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 product;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            float sum = 0;
            for (std::size_t index = 0; index < 4; ++index) {
                sum += a.element(row, index) * b.element(index, column);
            }
            product.values[(column * 4) + row] = sum;
        }
    }
    return product;
}

[[nodiscard]] constexpr Vec4 operator*(const Mat4& matrix, const Vec4& vector) noexcept {
    const std::array<float, 4> in{vector.x, vector.y, vector.z, vector.w};
    std::array<float, 4> out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            out[row] += matrix.element(row, column) * in[column];
        }
    }
    return {out[0], out[1], out[2], out[3]};
}

/// A point in world space, ready to be transformed.
[[nodiscard]] constexpr Vec4 homogeneous(core::Vec3 point) noexcept {
    return {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z),
            1};
}

/// The view transform of a camera at `eye` looking at `centre`.
///
/// Right-handed, with the camera looking down its own negative z axis, which is
/// the convention OpenGL's clip space is defined against.
///
/// `up` only has to be non-parallel to the direction of view; the component of
/// it along that direction is removed. A camera looking straight down its own up
/// vector has no defined orientation, and the caller is responsible for not
/// asking for one. `OrbitCamera` does that by clamping its elevation.
[[nodiscard]] Mat4 look_at(core::Vec3 eye, core::Vec3 centre, core::Vec3 up) noexcept;

/// The perspective projection of a symmetric frustum.
///
/// `vertical_field_of_view` is in radians and is the whole angle, not the half
/// angle. `near` and `far` are positive distances in front of the camera, and
/// both must be, since the transform divides by their difference and maps the
/// near plane to a depth that is only meaningful for a frustum in front of the
/// eye.
[[nodiscard]] Mat4 perspective(float vertical_field_of_view, float aspect_ratio, float near,
                               float far) noexcept;

} // namespace orrery::viz
