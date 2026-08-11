#include "orrery/viz/camera.hpp"

#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/viz/matrix4.hpp"

namespace {

using orrery::core::norm;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::viz::homogeneous;
using orrery::viz::Mat4;
using orrery::viz::OrbitCamera;
using orrery::viz::Vec4;

/// Loose enough for the single-precision build, where the matrices are the same
/// type as everything else and a product of two of them costs a few units in the
/// last place.
constexpr float kTolerance = 1e-5F;

[[nodiscard]] bool close_to(float measured, float expected) {
    return std::abs(measured - expected) < kTolerance * (1 + std::abs(expected));
}

/// The point after the perspective divide, which is what clip space means.
[[nodiscard]] Vec4 project(const Mat4& matrix, Vec3 point) {
    const Vec4 clip = matrix * homogeneous(point);
    return {clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, clip.w};
}

} // namespace

TEST_CASE("the camera is where its angles put it", "[unit][viz]") {
    OrbitCamera camera;
    camera.target = Vec3{1, 2, 3};
    camera.distance = 10;

    SECTION("along the x axis when both angles are zero") {
        camera.azimuth = 0;
        camera.elevation = 0;
        const Vec3 eye = camera.eye();
        CHECK(close_to(static_cast<float>(eye.x), 11.0F));
        CHECK(close_to(static_cast<float>(eye.y), 2.0F));
        CHECK(close_to(static_cast<float>(eye.z), 3.0F));
    }

    SECTION("overhead at the top of its range") {
        camera.elevation = std::numbers::pi_v<Real> / 2;
        const Vec3 eye = camera.eye();
        CHECK(close_to(static_cast<float>(eye.z), 13.0F));
    }

    // Whatever the angles, the camera is at the distance it says it is. This is
    // the invariant the orbit control has to preserve and the one a mistake in
    // the spherical coordinates would break.
    SECTION("always at the stated distance") {
        for (const Real azimuth :
             {static_cast<Real>(0), static_cast<Real>(1), static_cast<Real>(-2.5)}) {
            for (const Real elevation :
                 {static_cast<Real>(-1), static_cast<Real>(0), static_cast<Real>(1.2)}) {
                camera.azimuth = azimuth;
                camera.elevation = elevation;
                CHECK(close_to(static_cast<float>(norm(camera.eye() - camera.target)), 10.0F));
            }
        }
    }
}

TEST_CASE("the view transform puts the camera at the origin looking down negative z",
          "[unit][viz]") {
    OrbitCamera camera;
    camera.target = Vec3{-4, 7, 1};
    camera.distance = 6;
    camera.azimuth = static_cast<Real>(0.9);
    camera.elevation = static_cast<Real>(0.4);

    const Mat4 view = camera.view();

    // The eye maps to the origin, which is the definition of a view transform.
    const Vec4 eye = view * homogeneous(camera.eye());
    CHECK(close_to(eye.x, 0));
    CHECK(close_to(eye.y, 0));
    CHECK(close_to(eye.z, 0));

    // The target maps onto the negative z axis at the camera's distance. Both
    // halves matter: the sign says the camera looks the way OpenGL expects, and
    // the magnitude says the transform is a rotation and a translation rather
    // than something that has scaled the world.
    const Vec4 target = view * homogeneous(camera.target);
    CHECK(close_to(target.x, 0));
    CHECK(close_to(target.y, 0));
    CHECK(close_to(target.z, -6.0F));
}

TEST_CASE("the projection maps the frustum onto the cube", "[unit][viz]") {
    OrbitCamera camera;
    camera.distance = 10;
    camera.near_fraction = static_cast<Real>(0.1);
    camera.far_fraction = 2;

    const Mat4 projection = camera.projection(1);

    // A point on the near plane goes to a depth of minus one and one on the far
    // plane to plus one. The camera looks down its own negative z, so both are
    // at negative z in view space, which is the sign that is most often wrong.
    const Vec4 at_near = project(projection, Vec3{0, 0, -1});
    const Vec4 at_far = project(projection, Vec3{0, 0, -20});
    CHECK(at_near.z < -1 + kTolerance);
    CHECK(at_near.z > -1 - kTolerance);
    CHECK(at_far.z < 1 + kTolerance);
    CHECK(at_far.z > 1 - kTolerance);

    // The perspective divide is by the distance from the eye, so `w` is that
    // distance and not something else.
    CHECK(close_to(at_far.w, 20.0F));

    // The top of the frustum is at the tangent of half the field of view, and a
    // point there lands exactly on the edge of the cube.
    const Real half_height = 5 * std::tan(camera.field_of_view / 2);
    const Vec4 edge = project(projection, Vec3{0, half_height, -5});
    CHECK(close_to(edge.y, 1.0F));
}

TEST_CASE("a wide frame stretches the horizontal axis and not the vertical", "[unit][viz]") {
    // The field of view is the vertical one, so a wider window shows more to the
    // sides rather than magnifying everything. A projection that had the aspect
    // ratio the other way up would pass every test above and squash the picture.
    OrbitCamera camera;
    camera.distance = 10;

    const Real half_height = 5 * std::tan(camera.field_of_view / 2);
    const Mat4 wide = camera.projection(2);

    const Vec4 vertical = project(wide, Vec3{0, half_height, -5});
    CHECK(close_to(vertical.y, 1.0F));

    const Vec4 horizontal = project(wide, Vec3{half_height, 0, -5});
    CHECK(close_to(horizontal.x, 0.5F));
}

TEST_CASE("the controls stay inside the range the camera can represent", "[unit][viz]") {
    OrbitCamera camera;

    SECTION("elevation is clamped short of the pole") {
        camera.orbit(0, 100);
        CHECK(camera.elevation == OrbitCamera::kMaximumElevation);

        camera.orbit(0, -200);
        CHECK(camera.elevation == -OrbitCamera::kMaximumElevation);

        // Short of the pole rather than at it. At the pole the direction of view
        // is parallel to the up vector and the view transform has no defined
        // orientation.
        CHECK(OrbitCamera::kMaximumElevation < std::numbers::pi_v<Real> / 2);
    }

    SECTION("zooming is multiplicative and cannot reach the target") {
        camera.distance = 10;
        camera.zoom(static_cast<Real>(0.5));
        CHECK(close_to(static_cast<float>(camera.distance), 5.0F));

        for (int step = 0; step < 200; ++step) {
            camera.zoom(static_cast<Real>(0.5));
        }
        CHECK(camera.distance > 0);
    }

    SECTION("a zoom factor that is not a distance is ignored") {
        camera.distance = 10;
        camera.zoom(0);
        camera.zoom(-1);
        CHECK(close_to(static_cast<float>(camera.distance), 10.0F));
    }
}

TEST_CASE("panning moves the target across the view and not along it", "[unit][viz]") {
    OrbitCamera camera;
    camera.target = Vec3{};
    camera.distance = 10;
    camera.azimuth = static_cast<Real>(0.6);
    camera.elevation = static_cast<Real>(0.3);

    const Vec3 before = camera.eye() - camera.target;
    camera.pan(static_cast<Real>(0.25), static_cast<Real>(-0.1));

    // The camera keeps its distance and its orientation: panning slides what is
    // being looked at, it does not orbit or zoom. So the vector from the target
    // to the eye is unchanged and only the target has moved.
    const Vec3 after = camera.eye() - camera.target;
    CHECK(close_to(static_cast<float>(norm(after - before)), 0));
    CHECK(norm(camera.target) > 0);

    // And it moved in the plane of the screen, so it is still the same distance
    // along the direction of view.
    const Vec3 direction = before / norm(before);
    CHECK(close_to(static_cast<float>(dot(camera.target, direction)), 0));
}
