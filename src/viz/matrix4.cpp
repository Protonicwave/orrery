#include "orrery/viz/matrix4.hpp"

#include <cmath>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace orrery::viz {

namespace {

/// A world-space vector as three floats, normalised.
///
/// The simulation's scalar type may be double, and the camera's is not. The
/// conversion happens here, on a direction rather than on a position, so that
/// whatever is lost is lost from a unit vector where the absolute error is the
/// relative error rather than from a coordinate that may be far from the origin.
[[nodiscard]] core::Vec3 unit(core::Vec3 v) noexcept {
    const core::Real length = core::norm(v);
    return length > 0 ? v / length : v;
}

[[nodiscard]] float scalar(core::Real value) noexcept {
    return static_cast<float>(value);
}

} // namespace

Mat4 look_at(core::Vec3 eye, core::Vec3 centre, core::Vec3 up) noexcept {
    // The camera looks down its own negative z, so the basis vector that points
    // backwards out of the screen is from the target towards the eye.
    const core::Vec3 backward = unit(eye - centre);
    const core::Vec3 right = unit(core::cross(up, backward));

    // Recomputed from the two axes already fixed rather than taken from the
    // caller, which is what removes any component of `up` along the direction of
    // view and leaves an orthonormal basis whatever was passed in.
    const core::Vec3 above = core::cross(backward, right);

    // The rotation is the transpose of the basis, because the view transform
    // takes world coordinates into the camera's frame rather than the other way
    // round, and the translation is the eye position expressed in that frame.
    return Mat4{{scalar(right.x), scalar(above.x), scalar(backward.x), 0, scalar(right.y),
                 scalar(above.y), scalar(backward.y), 0, scalar(right.z), scalar(above.z),
                 scalar(backward.z), 0, scalar(-core::dot(right, eye)),
                 scalar(-core::dot(above, eye)), scalar(-core::dot(backward, eye)), 1}};
}

Mat4 perspective(float vertical_field_of_view, float aspect_ratio, float near, float far) noexcept {
    const float focal = 1.0F / std::tan(vertical_field_of_view / 2);

    // The standard OpenGL frustum. The third column is negative because the
    // camera looks down negative z while clip space increases into the screen,
    // and the fourth row copies negated z into w, which is what makes the
    // perspective divide a division by the distance from the eye.
    return Mat4{{focal / aspect_ratio, 0, 0, 0, 0, focal, 0, 0, 0, 0, (far + near) / (near - far),
                 -1, 0, 0, 2 * far * near / (near - far), 0}};
}

} // namespace orrery::viz
