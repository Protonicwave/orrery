#include "orrery/initial_conditions/kepler.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"

namespace {

using orrery::core::Diagnostics;
using orrery::core::dot;
using orrery::core::kGravitationalConstant;
using orrery::core::measure_diagnostics;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::kepler_angular_momentum;
using orrery::initial_conditions::kepler_energy;
using orrery::initial_conditions::kepler_periapsis_distance;
using orrery::initial_conditions::kepler_period;
using orrery::initial_conditions::KeplerParameters;
using orrery::initial_conditions::make_kepler_orbit;

/// The orbits every test below runs over.
///
/// Chosen to separate the cases that a wrong formula could still pass: equal
/// and very unequal masses, circular and strongly eccentric orbits, and a
/// semi-major axis away from one so that a missing power of it would show.
[[nodiscard]] std::vector<KeplerParameters> orbits() {
    return {
        {.primary_mass = 1, .secondary_mass = 1, .semi_major_axis = 1, .eccentricity = 0},
        {.primary_mass = 1,
         .secondary_mass = 1,
         .semi_major_axis = 1,
         .eccentricity = static_cast<Real>(0.5)},
        {.primary_mass = 1,
         .secondary_mass = 1,
         .semi_major_axis = 1,
         .eccentricity = static_cast<Real>(0.9)},
        {.primary_mass = 3,
         .secondary_mass = 2,
         .semi_major_axis = 7,
         .eccentricity = static_cast<Real>(0.25)},
        {.primary_mass = 1000,
         .secondary_mass = static_cast<Real>(0.001),
         .semi_major_axis = static_cast<Real>(0.4),
         .eccentricity = static_cast<Real>(0.7)},
    };
}

/// The energy of a Kepler orbit is a small difference between a large kinetic
/// and a large potential term, and the difference grows more delicate as the
/// orbit becomes more eccentric. A tolerance stated as a fraction of the energy
/// alone would be an assertion about the eccentricity rather than about the
/// code, so the scale of the terms that cancelled is carried explicitly.
[[nodiscard]] Real energy_tolerance(const Diagnostics& measured) {
    return 64 * std::numeric_limits<Real>::epsilon() *
           (measured.kinetic_energy + std::abs(measured.potential_energy));
}

} // namespace

TEST_CASE("the generated orbit is two bodies at periapsis about a centre at rest",
          "[unit][initial-conditions]") {
    for (const KeplerParameters& parameters : orbits()) {
        CAPTURE(parameters.primary_mass, parameters.secondary_mass, parameters.semi_major_axis,
                parameters.eccentricity);

        const ParticleData data = make_kepler_orbit(parameters);

        REQUIRE(data.size() == 2);
        REQUIRE(data.masses()[0] == parameters.primary_mass);
        REQUIRE(data.masses()[1] == parameters.secondary_mass);

        const Vec3 separation = data.positions().get(1) - data.positions().get(0);
        const Real expected = kepler_periapsis_distance(parameters);
        const Real tolerance = 8 * std::numeric_limits<Real>::epsilon() * expected;
        CAPTURE(norm(separation), expected);
        REQUIRE(std::abs(norm(separation) - expected) <= tolerance);

        // At periapsis the motion is entirely transverse. This is what makes
        // the starting state exact: anywhere else on the orbit it would have to
        // be found by solving Kepler's equation.
        const Vec3 relative_velocity = data.velocities().get(1) - data.velocities().get(0);
        REQUIRE(std::abs(dot(separation, relative_velocity)) <=
                8 * std::numeric_limits<Real>::epsilon() * norm(separation) *
                    norm(relative_velocity));

        const Diagnostics measured = measure_diagnostics(data, Softening{});

        // The configuration is built so that the weighted positions and
        // velocities cancel, so the centre of mass is at the origin and at rest
        // to within the one rounding that separates m1(m2/M) from m2(m1/M).
        const Real momentum_tolerance = 8 * std::numeric_limits<Real>::epsilon() *
                                        (parameters.primary_mass + parameters.secondary_mass) *
                                        norm(relative_velocity);
        CAPTURE(norm(measured.linear_momentum), momentum_tolerance);
        REQUIRE(norm(measured.linear_momentum) <= momentum_tolerance);
    }
}

TEST_CASE("the measured energy is the energy the elements imply",
          "[validation][initial-conditions]") {
    // The first of the analytic comparisons this project is built to make. The
    // total energy of a two-body orbit depends only on the semi-major axis and
    // the masses, and the diagnostic that measures it shares no code with the
    // generator that produced the state.
    for (const KeplerParameters& parameters : orbits()) {
        CAPTURE(parameters.primary_mass, parameters.secondary_mass, parameters.semi_major_axis,
                parameters.eccentricity);

        const ParticleData data = make_kepler_orbit(parameters);
        const Diagnostics measured = measure_diagnostics(data, Softening{});

        const Real expected = kepler_energy(parameters);
        const Real tolerance = energy_tolerance(measured);
        CAPTURE(measured.total_energy(), expected, tolerance);
        REQUIRE(std::abs(measured.total_energy() - expected) <= tolerance);
    }
}

TEST_CASE("the measured angular momentum is the one the elements imply",
          "[validation][initial-conditions]") {
    // The energy fixes the size of the orbit and this fixes its shape, so the
    // two together say the generated state is on the intended ellipse rather
    // than on some other one of the same extent.
    for (const KeplerParameters& parameters : orbits()) {
        CAPTURE(parameters.primary_mass, parameters.secondary_mass, parameters.semi_major_axis,
                parameters.eccentricity);

        const ParticleData data = make_kepler_orbit(parameters);
        const Diagnostics measured = measure_diagnostics(data, Softening{});

        const Real expected = kepler_angular_momentum(parameters);
        const Real tolerance = 32 * std::numeric_limits<Real>::epsilon() * expected;
        CAPTURE(norm(measured.angular_momentum), expected, tolerance);
        REQUIRE(std::abs(norm(measured.angular_momentum) - expected) <= tolerance);

        // The orbit lies in the x-y plane, so its angular momentum is along z
        // and the other two components are zero rather than small.
        REQUIRE(measured.angular_momentum.x == Real{0});
        REQUIRE(measured.angular_momentum.y == Real{0});
    }
}

TEST_CASE("the orbital period follows from the measured state",
          "[validation][initial-conditions]") {
    // The period cannot be measured without an integrator, which arrives in
    // Phase 4. What can be checked now is the step the measurement will depend
    // on: the semi-major axis recovered from the measured energy, and the
    // period that Kepler's third law gives from it. A generator that produced a
    // state on the wrong orbit would show up here as a period that does not
    // match the one its own elements ask for.
    for (const KeplerParameters& parameters : orbits()) {
        CAPTURE(parameters.primary_mass, parameters.secondary_mass, parameters.semi_major_axis,
                parameters.eccentricity);

        const ParticleData data = make_kepler_orbit(parameters);
        const Diagnostics measured = measure_diagnostics(data, Softening{});

        const Real total_mass = parameters.primary_mass + parameters.secondary_mass;
        const Real recovered_axis = -kGravitationalConstant * parameters.primary_mass *
                                    parameters.secondary_mass / (2 * measured.total_energy());
        const Real recovered_period = 2 * std::numbers::pi_v<Real> *
                                      std::sqrt(recovered_axis * recovered_axis * recovered_axis /
                                                (kGravitationalConstant * total_mass));

        // The period goes as the three-halves power of the axis, and the axis
        // inherits the relative error of the energy, so the relative error of
        // the period is one and a half times that of the energy.
        const Real relative_energy_error =
            energy_tolerance(measured) / std::abs(measured.total_energy());
        const Real tolerance = 2 * relative_energy_error * kepler_period(parameters);

        CAPTURE(recovered_period, kepler_period(parameters), tolerance);
        REQUIRE(std::abs(recovered_period - kepler_period(parameters)) <= tolerance);
    }
}

TEST_CASE("a circular orbit has a constant separation and a balanced speed",
          "[validation][initial-conditions]") {
    // The one orbit whose whole trajectory can be checked from a single state:
    // at zero eccentricity the separation is the semi-major axis everywhere,
    // and the virial ratio of a circular orbit is exactly one, since the
    // kinetic energy is half the magnitude of the potential energy at every
    // point rather than only on average.
    constexpr KeplerParameters kCircular{
        .primary_mass = 2, .secondary_mass = 1, .semi_major_axis = 5, .eccentricity = 0};

    const ParticleData data = make_kepler_orbit(kCircular);
    const Diagnostics measured = measure_diagnostics(data, Softening{});

    const Vec3 separation = data.positions().get(1) - data.positions().get(0);
    const Real tolerance = 8 * std::numeric_limits<Real>::epsilon() * kCircular.semi_major_axis;
    REQUIRE(std::abs(norm(separation) - kCircular.semi_major_axis) <= tolerance);

    const Real virial_tolerance = 32 * std::numeric_limits<Real>::epsilon();
    CAPTURE(measured.virial_ratio());
    REQUIRE(std::abs(measured.virial_ratio() - Real{1}) <= virial_tolerance);
}

TEST_CASE("the period does not depend on the eccentricity", "[validation][initial-conditions]") {
    // Kepler's third law, which is a statement worth checking on its own: two
    // orbits of the same semi-major axis and the same masses take the same time
    // however different their shapes.
    constexpr KeplerParameters kCircular{
        .primary_mass = 1, .secondary_mass = 1, .semi_major_axis = 2, .eccentricity = 0};
    constexpr KeplerParameters kEccentric{.primary_mass = 1,
                                          .secondary_mass = 1,
                                          .semi_major_axis = 2,
                                          .eccentricity = static_cast<Real>(0.95)};

    REQUIRE(kepler_period(kCircular) == kepler_period(kEccentric));
    REQUIRE(kepler_energy(kCircular) == kepler_energy(kEccentric));

    // Their angular momenta differ, which is what distinguishes them.
    REQUIRE(kepler_angular_momentum(kEccentric) < kepler_angular_momentum(kCircular));
}

TEST_CASE("elements that describe no ellipse are rejected", "[unit][initial-conditions]") {
    // Initial conditions are a setup boundary, where the conventions permit an
    // exception. The alternative, returning a configuration built from a NaN,
    // would surface much later as an integration that produced nothing and
    // would take far longer to trace back here.
    REQUIRE_THROWS_AS(make_kepler_orbit({.primary_mass = 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(make_kepler_orbit({.secondary_mass = -1}), std::invalid_argument);
    REQUIRE_THROWS_AS(make_kepler_orbit({.semi_major_axis = 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(make_kepler_orbit({.eccentricity = 1}), std::invalid_argument);
    REQUIRE_THROWS_AS(make_kepler_orbit({.eccentricity = static_cast<Real>(-0.1)}),
                      std::invalid_argument);

    // A NaN passes both halves of an ordinary range check, which is why the
    // generator states its conditions as assertions to be satisfied rather than
    // as failures to be detected.
    REQUIRE_THROWS_AS(make_kepler_orbit({.eccentricity = std::numeric_limits<Real>::quiet_NaN()}),
                      std::invalid_argument);

    // The analytic quantities reject the same elements, so that a caller cannot
    // ask for the period of an orbit that could not be built.
    REQUIRE_THROWS_AS(kepler_period({.eccentricity = 2}), std::invalid_argument);
    REQUIRE_THROWS_AS(kepler_energy({.semi_major_axis = -1}), std::invalid_argument);
    REQUIRE_THROWS_AS(kepler_angular_momentum({.primary_mass = 0}), std::invalid_argument);
    REQUIRE_THROWS_AS(kepler_periapsis_distance({.secondary_mass = 0}), std::invalid_argument);
}
