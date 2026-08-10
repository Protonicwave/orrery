#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "integrator_fixtures.hpp"
#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/integrators/runge_kutta4.hpp"

namespace {

using orrery::core::Diagnostics;
using orrery::core::Index;
using orrery::core::measure_diagnostics;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::integrators::Integrator;
using orrery::integrators::refresh_accelerations;
using orrery::integrators::RungeKutta4;
using orrery::testing::all_integrators;
using orrery::testing::CountingDirectField;
using orrery::testing::name_of;
using orrery::testing::symmetric_integrators;

/// The seed every run in this file starts from.
///
/// Fixed and reported in each failure message, as the conventions require: a
/// property test that cannot be re-run on the configuration that failed it is
/// not evidence of anything.
constexpr std::uint64_t kSeed = 20260810;

constexpr Index kParticles = 64;
constexpr Index kSteps = 200;

/// A softening length of a twentieth of the scale radius.
///
/// Small enough that the configuration is still a Plummer sphere rather than a
/// blur, large enough that a close pair among sixty-four particles cannot
/// produce an acceleration the fixed timestep below could not follow. The
/// conservation being measured is a property of the method, and a run that blew
/// up on one unlucky encounter would be measuring the sampling instead.
constexpr Real kSofteningLength = static_cast<Real>(0.03);

constexpr Real kTimestep = static_cast<Real>(0.001);

[[nodiscard]] ParticleData sampled_cluster() {
    RandomSource random{kSeed};
    return make_plummer_sphere({.count = kParticles}, random);
}

/// Integrate the cluster and return what it started and ended with.
struct Interval {
    Diagnostics before;
    Diagnostics after;
};

[[nodiscard]] Interval integrate(Integrator& integrator) {
    ParticleData data = sampled_cluster();
    const Softening softening{kSofteningLength};
    CountingDirectField field{softening};

    const Diagnostics before = measure_diagnostics(data, softening);

    refresh_accelerations(data, field);

    for (Index step = 0; step < kSteps; ++step) {
        integrator.step(data, kTimestep, field);
    }

    return {.before = before, .after = measure_diagnostics(data, softening)};
}

/// The round-off a conserved sum can accumulate over the run.
///
/// Each step adds an error of the order of one rounding of the quantity itself,
/// and the errors accumulate at worst linearly, so the bound is proportional to
/// the number of steps rather than to its square root. `scale` is the size of the
/// terms being summed, which is what a relative rounding is relative to.
[[nodiscard]] Real round_off_bound(Real scale) {
    return 64 * static_cast<Real>(kSteps) * std::numeric_limits<Real>::epsilon() * scale;
}

} // namespace

TEST_CASE("linear momentum is conserved to round-off", "[property][integrators]") {
    // The property every method here holds exactly, and the one that catches an
    // asymmetric force accumulation faster than anything else. Newton's third law
    // makes the momentum change of a pair cancel between its two members, so the
    // total cannot move except by rounding, whatever the timestep and whatever
    // the configuration. RK4 keeps it for the same reason the symplectic methods
    // do: the total is a linear function of the state, and every stage of every
    // method here changes it by a sum that cancels term by term.
    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator), kSeed, kParticles, kSteps);

        const Interval interval = integrate(*integrator);
        const Vec3 drift = interval.after.linear_momentum - interval.before.linear_momentum;

        // The scale of the terms that cancelled, not of the total, which is very
        // nearly zero. A tolerance relative to a quantity that is itself zero
        // would be a tolerance of zero.
        const Real scale = std::sqrt(2 * interval.before.kinetic_energy);
        const Real tolerance = round_off_bound(scale);

        CAPTURE(norm(drift), tolerance);
        REQUIRE(norm(drift) <= tolerance);
    }
}

TEST_CASE("the symplectic methods conserve angular momentum to round-off",
          "[property][integrators]") {
    // A stronger statement than the one above, and one that separates the two
    // families again. Every kick changes the angular momentum by a sum of terms
    // in which a separation vector is crossed with a force parallel to it, and
    // every drift by a velocity crossed with itself, so both halves of a leapfrog
    // step contribute exactly nothing. Yoshida's composition inherits it.
    //
    // RK4 has no such argument available to it: angular momentum is quadratic in
    // the state, and the stage combination that makes the method fourth order
    // does not preserve quadratic invariants. It is checked below rather than
    // here, and against a far weaker bound.
    for (const std::unique_ptr<Integrator>& integrator : symmetric_integrators()) {
        CAPTURE(name_of(*integrator), kSeed, kParticles, kSteps);

        const Interval interval = integrate(*integrator);
        const Vec3 drift = interval.after.angular_momentum - interval.before.angular_momentum;

        const Real scale = norm(interval.before.angular_momentum);
        const Real tolerance = round_off_bound(scale);

        CAPTURE(norm(drift), scale, tolerance);
        REQUIRE(norm(drift) <= tolerance);
    }
}

TEST_CASE("RK4 conserves angular momentum only to its truncation error",
          "[property][integrators]") {
    // Not a defect, and not asserted as a tight bound: the point is that the
    // conservation RK4 offers is a consequence of its accuracy rather than of its
    // structure, so it degrades with the timestep where the symplectic methods'
    // does not. Over a short run at a small step the error is small in absolute
    // terms, which is why the bound here is stated against the size of the
    // quantity rather than against round-off.
    RungeKutta4 integrator;

    const Interval interval = integrate(integrator);
    const Vec3 drift = interval.after.angular_momentum - interval.before.angular_momentum;
    const Real scale = norm(interval.before.angular_momentum);

    CAPTURE(kSeed, kParticles, kSteps, norm(drift), scale);
    REQUIRE(norm(drift) <= scale / 1000);
}
