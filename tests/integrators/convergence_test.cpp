#include <cmath>
#include <memory>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "integrator_fixtures.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/kepler.hpp"
#include "orrery/integrators/integrator.hpp"
#include "orrery/integrators/yoshida4.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::initial_conditions::kepler_period;
using orrery::initial_conditions::KeplerParameters;
using orrery::initial_conditions::make_kepler_orbit;
using orrery::integrators::Integrator;
using orrery::integrators::refresh_accelerations;
using orrery::integrators::Yoshida4;
using orrery::testing::all_integrators;
using orrery::testing::CountingDirectField;
using orrery::testing::name_of;

/// A circular orbit, whose exact solution at any time is a rotation of its
/// initial state.
///
/// This is what makes it the right instrument for measuring a convergence order.
/// On an eccentric orbit the exact state after a given time can only be found by
/// solving Kepler's equation, and the iteration that solves it would put its own
/// error inside the quantity being measured. On a circular orbit the state after
/// one full period is the state it started in, so the error after a period can be
/// compared against the starting configuration and nothing else.
constexpr KeplerParameters kCircularOrbit{
    .primary_mass = 1, .secondary_mass = 1, .semi_major_axis = 1, .eccentricity = 0};

/// The largest distance any particle ends up from where it began, after one
/// orbital period integrated in `steps` equal steps.
///
/// The error is dominated by the phase of the orbit rather than by its radius: a
/// symplectic method keeps a circular orbit very nearly circular and very nearly
/// the right size, and gets to the wrong place along it. That is the error a
/// convergence order is a statement about.
[[nodiscard]] Real closure_error(Integrator& integrator, Index steps) {
    const ParticleData start = make_kepler_orbit(kCircularOrbit);
    ParticleData data = start;
    CountingDirectField field;

    const Real timestep = kepler_period(kCircularOrbit) / static_cast<Real>(steps);

    refresh_accelerations(data, field);

    for (Index step = 0; step < steps; ++step) {
        integrator.step(data, timestep, field);
    }

    Real worst = 0;

    for (Index particle = 0; particle < data.size(); ++particle) {
        const Real error = norm(data.positions().get(particle) - start.positions().get(particle));
        worst = error > worst ? error : worst;
    }

    return worst;
}

/// The coarser of the two step counts a method's order is measured between.
///
/// Two constraints squeeze this number from both sides. Too few steps and the
/// method is not yet in the regime where its error is a clean power of the
/// timestep; too many and the discretisation error falls into the round-off
/// floor, where halving the step stops helping and the measured order collapses.
/// The gap between the two is generous in double precision and narrow in single,
/// where the floor sits eight decades higher, so the single-precision build
/// measures the same thing over shorter runs and judges it less tightly.
[[nodiscard]] constexpr Index coarse_steps(int order) {
    if constexpr (kSinglePrecision) {
        return order == 2 ? 48 : 32;
    } else {
        return order == 2 ? 256 : 128;
    }
}

/// How far the measured order may sit from the stated one.
[[nodiscard]] constexpr Real order_tolerance() {
    return kSinglePrecision ? static_cast<Real>(0.6) : static_cast<Real>(0.2);
}

} // namespace

TEST_CASE("each method converges at the order it claims", "[validation][integrators]") {
    // The first of this phase's two headline results. Halving the timestep
    // divides the error of a method of order p by 2^p, so the ratio of the
    // errors at two step sizes recovers p from a measurement. A method whose
    // arithmetic was subtly wrong would still integrate something and would
    // still produce plausible orbits; it would not produce the right power.
    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator), integrator->order());

        const Index coarse = coarse_steps(integrator->order());
        const Real coarse_error = closure_error(*integrator, coarse);
        const Real fine_error = closure_error(*integrator, 2 * coarse);

        CAPTURE(coarse, coarse_error, fine_error);
        REQUIRE(fine_error > 0);
        REQUIRE(fine_error < coarse_error);

        const Real measured_order = std::log2(coarse_error / fine_error);
        CAPTURE(measured_order, order_tolerance());
        REQUIRE(std::abs(measured_order - static_cast<Real>(integrator->order())) <=
                order_tolerance());
    }
}

TEST_CASE("the integrated orbit takes the period Kepler's third law gives it",
          "[validation][integrators]") {
    // The measurement Phase 3 could set up but not make. The configuration
    // starts at periapsis on the positive x axis, so the relative separation has
    // zero y component there and returns to zero y once per orbit. Timing that
    // return and comparing it against `2 pi sqrt(a^3 / G M)` closes the loop:
    // the period is a property of the physics, the analytic value comes from the
    // elements, and the measured one comes from an integration that knows
    // nothing about either.
    constexpr KeplerParameters kOrbit{.primary_mass = 2,
                                      .secondary_mass = 1,
                                      .semi_major_axis = static_cast<Real>(1.5),
                                      .eccentricity = static_cast<Real>(0.5)};
    constexpr Index kStepsPerOrbit = 2000;

    const Real expected = kepler_period(kOrbit);
    const Real timestep = expected / static_cast<Real>(kStepsPerOrbit);

    ParticleData data = make_kepler_orbit(kOrbit);
    CountingDirectField field;
    Yoshida4 integrator;
    refresh_accelerations(data, field);

    const auto separation_y = [&data]() {
        return data.positions().get(1).y - data.positions().get(0).y;
    };

    // Step until the separation crosses the axis from below, which happens once
    // per orbit and only at periapsis. The first few steps leave y positive, so
    // the crossing found is the completion of a full orbit rather than the start
    // of it.
    Real previous_y = separation_y();
    Real measured = 0;

    for (Index step = 1; step <= 2 * kStepsPerOrbit; ++step) {
        integrator.step(data, timestep, field);
        const Real current_y = separation_y();

        if (previous_y < 0 && current_y >= 0) {
            // Linear interpolation between the two samples that straddle the
            // crossing. The separation is a smooth function of time, so this
            // costs an error of order h^2, which at two thousand steps an orbit
            // is far below the accuracy of the integration itself.
            const Real fraction = -previous_y / (current_y - previous_y);
            measured = (static_cast<Real>(step - 1) + fraction) * timestep;
            break;
        }

        previous_y = current_y;
    }

    // A tolerance of a part in ten thousand. The interpolation above is the
    // limiting term rather than the integration, and it is generous enough to
    // hold in single precision while still being violated many times over by any
    // real error in the period.
    const Real tolerance = expected / 10000;
    CAPTURE(measured, expected, tolerance);
    REQUIRE(measured > 0);
    REQUIRE(std::abs(measured - expected) <= tolerance);
}
