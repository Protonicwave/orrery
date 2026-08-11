#include "orrery/viz/camera.hpp"

#include <algorithm>
#include <cmath>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/viz/matrix4.hpp"

namespace orrery::viz {

namespace {

using core::Real;
using core::Vec3;

/// The smallest distance the camera will hold.
///
/// Not zero, because a camera at its target has no direction of view and the
/// projection would divide by nothing. Small enough that reaching it means
/// someone has scrolled a long way in on purpose.
constexpr Real kMinimumDistance = static_cast<Real>(1e-4);

/// Which way is up, for the view transform.
///
/// The z axis, fixed rather than carried in the camera's state. The elevation is
/// clamped away from the poles so the direction of view is never parallel to it,
/// and a fixed world up is what keeps the horizon level: a camera that carried
/// its own up vector and rotated it with the view would slowly roll as it was
/// dragged in circles, which is disorienting and has no undo.
constexpr Vec3 kWorldUp{0, 0, 1};

} // namespace

Vec3 OrbitCamera::eye() const noexcept {
    const Real horizontal = distance * std::cos(elevation);
    return target + Vec3{horizontal * std::cos(azimuth), horizontal * std::sin(azimuth),
                         distance * std::sin(elevation)};
}

Mat4 OrbitCamera::view() const noexcept {
    return look_at(eye(), target, kWorldUp);
}

Mat4 OrbitCamera::projection(Real aspect_ratio) const noexcept {
    return perspective(static_cast<float>(field_of_view), static_cast<float>(aspect_ratio),
                       static_cast<float>(distance * near_fraction),
                       static_cast<float>(distance * far_fraction));
}

Mat4 OrbitCamera::view_projection(Real aspect_ratio) const noexcept {
    // Projection then view, which in matrix order is the projection on the left:
    // a point is transformed into the camera's frame first and projected second.
    return projection(aspect_ratio) * view();
}

void OrbitCamera::orbit(Real delta_azimuth, Real delta_elevation) noexcept {
    azimuth += delta_azimuth;
    elevation = std::clamp(elevation + delta_elevation, -kMaximumElevation, kMaximumElevation);
}

void OrbitCamera::zoom(Real factor) noexcept {
    if (!(factor > 0)) {
        // A factor of zero or less is not a distance the camera can be at, and a
        // negative one would put it behind its own target. Ignored rather than
        // clamped, since there is no sensible interpretation to clamp towards.
        return;
    }
    distance = std::max(distance * factor, kMinimumDistance);
}

void OrbitCamera::pan(Real right, Real up) noexcept {
    // The height of the frame at the distance of the target. Panning by a
    // fraction of this rather than by an absolute length is what makes a drag of
    // a given number of pixels move the image by the same number of pixels
    // whatever the camera's distance.
    const Real height = 2 * distance * std::tan(field_of_view / 2);

    const Vec3 forward = (target - eye()) / distance;
    const Vec3 across = core::cross(forward, kWorldUp);
    const Vec3 screen_up = core::cross(across, forward);

    // Both are unit vectors already: `forward` is a difference divided by its own
    // length, and the two cross products are of perpendicular unit vectors. They
    // are normalised anyway, because `forward` is only unit to round-off and the
    // error would otherwise accumulate over a drag of several hundred frames.
    const Real across_length = core::norm(across);
    const Real up_length = core::norm(screen_up);
    if (!(across_length > 0) || !(up_length > 0)) {
        return;
    }

    target += (across / across_length * (right * height)) + (screen_up / up_length * (up * height));
}

} // namespace orrery::viz
