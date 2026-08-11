#pragma once

/// \file
/// The camera, as three angles and a distance.
///
/// An orbit camera rather than a free one. It holds a point it is looking at, a
/// distance from that point, and two angles about it, and every control moves
/// one of those four numbers. A free camera, with an arbitrary position and
/// orientation, is more general and is the wrong tool here: what someone looking
/// at a galaxy collision wants is to go round it and look closer, and a camera
/// that can also roll and fly off into empty space mostly offers new ways to
/// lose the thing being examined.
///
/// It is also what makes the offline export reproducible. Four numbers, three of
/// which are angles, can be written on a command line and produce the same frame
/// on any machine. A position and a quaternion cannot be typed by anyone.
///
/// ## Why this holds no OpenGL
///
/// Nothing in this file or its implementation calls a graphics function, so the
/// camera builds and is tested in a build with no renderer at all. That matters
/// for more than tidiness: the projection is where an off-by-one in a sign
/// convention hides, and a bug there shows up as a black window rather than as a
/// message, which is the worst kind of thing to debug through a device driver.
/// Here it is a function from four numbers to sixteen, and the tests treat it as
/// one.

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/viz/matrix4.hpp"

namespace orrery::viz {

/// A camera that orbits a point.
///
/// The angles follow the usual astronomical convention rather than a graphics
/// one: the azimuth is measured in the x-y plane from the positive x axis, and
/// the elevation up from that plane towards positive z. The simulation's own
/// spin axes are z axes, so a camera at zero elevation looks at a disc galaxy
/// edge on and one at a right angle looks at it face on, which is the vocabulary
/// the scenario is described in.
class OrbitCamera {
public:
    /// The largest angle from the plane the camera will take, in radians.
    ///
    /// A hundredth of a radian short of the pole. Exactly at the pole the
    /// direction of view is parallel to the up vector and the view transform has
    /// no defined orientation, which shows on screen as the image spinning
    /// through a right angle as the camera crosses it. Clamping is the ordinary
    /// remedy and the alternative, carrying an explicit up vector through the
    /// controls, buys a view nobody has asked for at the cost of a camera that
    /// can end up upside down with no obvious way back.
    static constexpr core::Real kMaximumElevation = static_cast<core::Real>(1.5607963);

    /// The point the camera looks at.
    core::Vec3 target;

    /// The distance from the target, which is what zooming changes.
    core::Real distance = 30;

    /// The angle in the x-y plane, in radians, measured from the positive x axis.
    core::Real azimuth = 0;

    /// The angle above the x-y plane, in radians.
    core::Real elevation = static_cast<core::Real>(0.3);

    /// The whole vertical angle the frustum spans, in radians.
    ///
    /// Forty degrees. Wide enough that a merging pair fills the frame without
    /// the perspective distortion a wider angle gives, which for a point cloud
    /// with no straight lines in it reads as the near side of the system being
    /// unaccountably larger than the far side.
    core::Real field_of_view = static_cast<core::Real>(0.6981317);

    /// The near and far clipping distances, as fractions of the distance to the
    /// target.
    ///
    /// Relative rather than absolute because the camera is used at scales from a
    /// single galaxy to a pair separated by twenty scale lengths, and a fixed
    /// near plane that suits one clips through the other. Keeping the ratio of
    /// far to near at a thousand also keeps the depth buffer usable, which
    /// matters even though the points are drawn without depth testing: a caller
    /// that turns it on should not find the precision already spent.
    core::Real near_fraction = static_cast<core::Real>(0.01);
    core::Real far_fraction = 10;

    /// Where the camera is, which follows from the target, the distance and the
    /// two angles.
    [[nodiscard]] core::Vec3 eye() const noexcept;

    [[nodiscard]] Mat4 view() const noexcept;

    [[nodiscard]] Mat4 projection(core::Real aspect_ratio) const noexcept;

    /// The product of the two, which is the only matrix the renderer needs.
    [[nodiscard]] Mat4 view_projection(core::Real aspect_ratio) const noexcept;

    /// Turn about the target, clamping the elevation away from the poles.
    void orbit(core::Real delta_azimuth, core::Real delta_elevation) noexcept;

    /// Move towards or away from the target by a multiplicative factor.
    ///
    /// Multiplicative rather than additive so that one notch of a scroll wheel
    /// covers the same fraction of the remaining distance wherever the camera
    /// is. An additive step is either uselessly small when far away or lands
    /// inside the target when near it.
    ///
    /// The distance is kept positive; a factor that would take it to zero or
    /// through it leaves it at the smallest value this camera will hold.
    void zoom(core::Real factor) noexcept;

    /// Slide the target across the plane of the screen.
    ///
    /// The arguments are fractions of the height of the frame at the distance of
    /// the target, so a pan of one moves the target by the full height of what
    /// is on screen whatever the camera's distance or field of view.
    void pan(core::Real right, core::Real up) noexcept;
};

} // namespace orrery::viz
