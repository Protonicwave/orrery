#include "orrery/integrators/integrator.hpp"

#include <limits>
#include <memory>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "integrator_fixtures.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_array.hpp"
#include "orrery/initial_conditions/kepler.hpp"
#include "orrery/integrators/runge_kutta4.hpp"
#include "orrery/integrators/velocity_verlet.hpp"
#include "orrery/integrators/yoshida4.hpp"

namespace {

using orrery::core::Index;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::core::Vec3Array;
using orrery::initial_conditions::KeplerParameters;
using orrery::initial_conditions::make_kepler_orbit;
using orrery::integrators::Integrator;
using orrery::integrators::refresh_accelerations;
using orrery::integrators::RungeKutta4;
using orrery::integrators::VelocityVerlet;
using orrery::integrators::Yoshida4;
using orrery::testing::all_integrators;
using orrery::testing::CountingDirectField;
using orrery::testing::name_of;
using orrery::testing::symmetric_integrators;

} // namespace

TEST_CASE("each integrator describes itself consistently", "[unit][integrators]") {
    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator));

        REQUIRE_FALSE(integrator->name().empty());
        REQUIRE(integrator->order() >= 2);
        REQUIRE(integrator->force_evaluations_per_step() >= 1);
    }

    // The two claims the rest of the suite turns on, stated plainly here so that
    // a change to either is a deliberate edit to a test rather than a silent
    // change of meaning elsewhere.
    REQUIRE(VelocityVerlet{}.is_symplectic());
    REQUIRE(Yoshida4{}.is_symplectic());
    REQUIRE_FALSE(RungeKutta4{}.is_symplectic());
}

TEST_CASE("a step costs the force evaluations the method says it does", "[unit][integrators]") {
    // The number every comparison between methods has to be normalised by. An
    // integrator that quietly evaluated the field more often than it reported
    // would make its accuracy per unit of work look better than it is, which is
    // the one measurement error this project cannot afford in a comparison it
    // publishes.
    constexpr KeplerParameters kOrbit{
        .primary_mass = 1, .secondary_mass = 1, .semi_major_axis = 1, .eccentricity = 0};
    constexpr Index kSteps = 5;

    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator));

        ParticleData data = make_kepler_orbit(kOrbit);
        CountingDirectField field;

        refresh_accelerations(data, field);
        REQUIRE(field.evaluations() == 1);

        field.reset_evaluations();

        for (Index step = 0; step < kSteps; ++step) {
            integrator->step(data, static_cast<Real>(0.01), field);
        }

        REQUIRE(field.evaluations() == kSteps * integrator->force_evaluations_per_step());
    }
}

TEST_CASE("a lone particle travels in a straight line", "[unit][integrators]") {
    // A particle with nothing to attract it feels no acceleration, so every
    // method here reduces to uniform motion and any of them that did not would
    // be adding something that is not in the equations. It is also the smallest
    // configuration that exercises the drift half of a step on its own.
    constexpr Vec3 kStart{1, -2, 3};
    constexpr Vec3 kVelocity{static_cast<Real>(0.25), static_cast<Real>(-0.5), 2};
    constexpr Real kStep = static_cast<Real>(0.125);
    constexpr Index kSteps = 40;

    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator));

        ParticleData data;
        data.add(kStart, kVelocity, 1);

        CountingDirectField field;
        refresh_accelerations(data, field);

        for (Index step = 0; step < kSteps; ++step) {
            integrator->step(data, kStep, field);
        }

        const Real elapsed = static_cast<Real>(kSteps) * kStep;
        const Vec3 expected = kStart + (elapsed * kVelocity);
        const Vec3 error = data.positions().get(0) - expected;

        // The position is reached by repeated addition rather than by one
        // multiplication, so the two agree to the rounding of that repetition
        // and not to the last bit.
        const Real tolerance =
            8 * static_cast<Real>(kSteps) * std::numeric_limits<Real>::epsilon() * norm(expected);
        CAPTURE(norm(error), tolerance);
        REQUIRE(norm(error) <= tolerance);
        REQUIRE(data.velocities().get(0) == kVelocity);
    }
}

TEST_CASE("an empty configuration steps without complaint", "[unit][integrators]") {
    // A simulation is allowed to have nothing in it, and the loops in every
    // method here have to be written so that the zero-length case falls out
    // rather than being special-cased. The sanitiser build is where this test
    // earns its place.
    ParticleData data;
    CountingDirectField field;

    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator));

        refresh_accelerations(data, field);
        integrator->step(data, static_cast<Real>(0.1), field);

        REQUIRE(data.empty());
    }
}

TEST_CASE("a step leaves the accelerations agreeing with the positions", "[unit][integrators]") {
    // The invariant the interface is built on, asserted rather than assumed.
    // Velocity Verlet is only a one-evaluation method because the acceleration it
    // starts from is the one the previous step left, and RK4's first stage costs
    // nothing for the same reason. An integrator that left a stale acceleration
    // behind, RK4 leaving its fourth stage's values would be the natural mistake,
    // would corrupt the next step in a way that no single-step test would see.
    constexpr KeplerParameters kOrbit{.primary_mass = 2,
                                      .secondary_mass = 1,
                                      .semi_major_axis = 1,
                                      .eccentricity = static_cast<Real>(0.3)};

    for (const std::unique_ptr<Integrator>& integrator : all_integrators()) {
        CAPTURE(name_of(*integrator));

        ParticleData data = make_kepler_orbit(kOrbit);
        CountingDirectField field;

        refresh_accelerations(data, field);
        integrator->step(data, static_cast<Real>(0.01), field);
        integrator->step(data, static_cast<Real>(0.01), field);

        // Exact equality is the right comparison here. The same field evaluated
        // at the same positions with the same masses produces the same bits, so
        // any difference at all is a stale value rather than a rounding.
        Vec3Array expected{data.size()};
        field.evaluate(data.positions(), data.masses(), expected.view());

        for (Index particle = 0; particle < data.size(); ++particle) {
            CAPTURE(particle);
            REQUIRE(data.accelerations().get(particle) == expected.view().get(particle));
        }
    }
}

TEST_CASE("the symmetric methods run backwards to where they started",
          "[validation][integrators]") {
    // Time reversibility is the structural property that distinguishes these two
    // methods from RK4, and it is exact rather than approximate: stepping
    // forward and then stepping back with a negative timestep is the identity in
    // exact arithmetic, whatever the timestep. A method that had a subtle
    // asymmetry between its two half-kicks would still converge at the right
    // order and would still look plausible on an energy plot, and this is the
    // test that would catch it.
    //
    // RK4 is deliberately not asserted here. It is not symmetric, and its
    // failure to return is a property rather than a defect.
    constexpr KeplerParameters kOrbit{.primary_mass = 3,
                                      .secondary_mass = 1,
                                      .semi_major_axis = 1,
                                      .eccentricity = static_cast<Real>(0.4)};
    constexpr Index kSteps = 200;
    const Real step = static_cast<Real>(0.002);

    for (const std::unique_ptr<Integrator>& integrator : symmetric_integrators()) {
        CAPTURE(name_of(*integrator));

        const ParticleData start = make_kepler_orbit(kOrbit);
        ParticleData data = start;
        CountingDirectField field{Softening{static_cast<Real>(0.01)}};

        refresh_accelerations(data, field);

        for (Index index = 0; index < kSteps; ++index) {
            integrator->step(data, step, field);
        }

        for (Index index = 0; index < kSteps; ++index) {
            integrator->step(data, -step, field);
        }

        // Round-off is not reversed by reversing time, so the tolerance grows
        // with the number of steps taken rather than being a fixed multiple of
        // epsilon.
        const Real tolerance =
            64 * static_cast<Real>(kSteps) * std::numeric_limits<Real>::epsilon();

        for (Index particle = 0; particle < data.size(); ++particle) {
            const Vec3 position_error =
                data.positions().get(particle) - start.positions().get(particle);
            const Vec3 velocity_error =
                data.velocities().get(particle) - start.velocities().get(particle);

            CAPTURE(particle, norm(position_error), norm(velocity_error), tolerance);
            REQUIRE(norm(position_error) <= tolerance);
            REQUIRE(norm(velocity_error) <= tolerance);
        }
    }
}
