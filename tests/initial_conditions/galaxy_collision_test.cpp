#include "orrery/initial_conditions/galaxy_collision.hpp"

#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/initial_conditions/disc_galaxy.hpp"

namespace {

using orrery::core::centre_of_mass;
using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::linear_momentum;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::core::Vec3Span;
using orrery::initial_conditions::disc_galaxy_total_mass;
using orrery::initial_conditions::DiscGalaxyParameters;
using orrery::initial_conditions::galaxy_collision_orbit_energy;
using orrery::initial_conditions::galaxy_collision_relative_velocity;
using orrery::initial_conditions::galaxy_collision_separation;
using orrery::initial_conditions::GalaxyCollisionParameters;
using orrery::initial_conditions::make_galaxy_collision;

constexpr std::uint64_t kSeed = 20260811;

constexpr DiscGalaxyParameters kPrimary{
    .count = 3000, .disc_mass = 1, .bulge_mass = static_cast<Real>(0.25)};

// Fewer particles than its share of the mass, so the two galaxies have
// different particle masses and the ordering of the combined array can be
// checked by reading them.
constexpr DiscGalaxyParameters kSecondary{.count = 1000,
                                          .disc_mass = static_cast<Real>(0.5),
                                          .bulge_mass = static_cast<Real>(0.125),
                                          .inclination = 1};

constexpr GalaxyCollisionParameters kEncounter{.primary = kPrimary, .secondary = kSecondary};

/// The centre of mass and its velocity for one half of the pair.
struct Body {
    Vec3 position;
    Vec3 velocity;
};

[[nodiscard]] Body body_of(const ParticleData& data, Index first, Index count) {
    const Vec3Span<const Real> positions = data.positions();
    const Vec3Span<const Real> velocities = data.velocities();
    const std::span<const Real> masses = data.masses();

    Body body;
    Real total = 0;
    for (Index particle = first; particle < first + count; ++particle) {
        body.position += positions.get(particle) * masses[particle];
        body.velocity += velocities.get(particle) * masses[particle];
        total += masses[particle];
    }
    body.position /= total;
    body.velocity /= total;
    return body;
}

} // namespace

TEST_CASE("an encounter holds both galaxies, primary first", "[unit][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_galaxy_collision(kEncounter, random);
    REQUIRE(data.size() == kPrimary.count + kSecondary.count);

    // The two galaxies carry different particle masses here, which is what makes
    // the ordering checkable: the first block should all be the primary's mass
    // and the second all the secondary's.
    const std::span<const Real> masses = data.masses();
    for (Index particle = 0; particle < kPrimary.count; ++particle) {
        REQUIRE(masses[particle] == masses[0]);
    }
    for (Index particle = kPrimary.count; particle < data.size(); ++particle) {
        REQUIRE(masses[particle] == masses[kPrimary.count]);
    }
    REQUIRE(masses[0] != masses[kPrimary.count]);
}

TEST_CASE("the two galaxies are placed on the encounter they were asked for",
          "[validation][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_galaxy_collision(kEncounter, random);

    const Body primary = body_of(data, 0, kPrimary.count);
    const Body secondary = body_of(data, kPrimary.count, kSecondary.count);

    // Each galaxy was centred to round-off before being placed, so the measured
    // separation is the requested one to the same accuracy. This is the check
    // that the placement uses the mass-weighted split rather than, say, putting
    // each galaxy at half the separation, which would be wrong for every mass
    // ratio except one.
    const Vec3 separation = secondary.position - primary.position;
    const Vec3 expected = galaxy_collision_separation(kEncounter);
    REQUIRE(norm(separation - expected) <
            static_cast<Real>(kSinglePrecision ? 1e-4 : 1e-6) * norm(expected));

    const Vec3 relative = secondary.velocity - primary.velocity;
    const Vec3 expected_velocity = galaxy_collision_relative_velocity(kEncounter);
    REQUIRE(norm(relative - expected_velocity) <
            static_cast<Real>(kSinglePrecision ? 1e-4 : 1e-6) * norm(expected_velocity));

    // The approach is along the negative x axis and the separation is not, so a
    // configuration that had aimed one galaxy at the other would fail here while
    // passing everything above.
    REQUIRE(relative.x < 0);
    REQUIRE(std::abs(relative.y) <
            static_cast<Real>(kSinglePrecision ? 1e-4 : 1e-6) * norm(expected_velocity));
}

TEST_CASE("the pair is at rest at the origin", "[property][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_galaxy_collision(kEncounter, random);

    constexpr Real kResidue = static_cast<Real>(kSinglePrecision ? 1e-5 : 1e-9);
    REQUIRE(norm(centre_of_mass(data.positions(), data.masses())) < kResidue);
    REQUIRE(norm(linear_momentum(data.velocities(), data.masses())) < kResidue);
}

TEST_CASE("an approach speed of one is a parabolic encounter", "[validation][initial-conditions]") {
    // The analytic statement the approach speed parameter makes. At the escape
    // speed the two-body energy of the pair is exactly zero, so the encounter is
    // marginally unbound; below it the energy is negative and the pair merges,
    // above it positive and they separate for ever. Checking all three is what
    // makes the parameter mean what its documentation says rather than merely
    // scale something.
    GalaxyCollisionParameters parameters = kEncounter;

    parameters.approach_speed = 1;
    const Real scale = disc_galaxy_total_mass(kPrimary) * disc_galaxy_total_mass(kSecondary) /
                       norm(galaxy_collision_separation(parameters));
    REQUIRE(std::abs(galaxy_collision_orbit_energy(parameters)) <
            static_cast<Real>(kSinglePrecision ? 1e-4 : 1e-6) * scale);

    parameters.approach_speed = static_cast<Real>(0.8);
    REQUIRE(galaxy_collision_orbit_energy(parameters) < 0);

    parameters.approach_speed = static_cast<Real>(1.2);
    REQUIRE(galaxy_collision_orbit_energy(parameters) > 0);
}

TEST_CASE("an encounter refuses a geometry it cannot place", "[unit][initial-conditions]") {
    RandomSource random{kSeed};

    GalaxyCollisionParameters parameters = kEncounter;
    parameters.separation = 0;
    parameters.impact_parameter = 0;
    REQUIRE_THROWS_AS(make_galaxy_collision(parameters, random), std::invalid_argument);

    parameters = kEncounter;
    parameters.approach_speed = -1;
    REQUIRE_THROWS_AS(make_galaxy_collision(parameters, random), std::invalid_argument);

    // A galaxy that cannot be sampled stops the encounter as well, rather than
    // being caught only when the renderer finds half a configuration.
    parameters = kEncounter;
    parameters.secondary.scale_length = 0;
    REQUIRE_THROWS_AS(make_galaxy_collision(parameters, random), std::invalid_argument);
}
