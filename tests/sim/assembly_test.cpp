#include "orrery/sim/assembly.hpp"

#include <algorithm>
#include <cmath>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/initial_conditions/disc_galaxy.hpp"
#include "orrery/sim/configuration.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::total_mass;
using orrery::core::Vec3;
using orrery::sim::Configuration;
using orrery::sim::InitialConditionKind;
using orrery::sim::make_initial_conditions;

/// A configuration for one of the two galaxy scenarios, with everything the
/// scenario does not decide left at its default.
[[nodiscard]] Configuration galaxy_configuration(InitialConditionKind kind, Index count) {
    Configuration configuration;
    configuration.run.timestep = static_cast<Real>(0.01);
    configuration.run.steps = 1;
    configuration.run.seed = 20260811;
    configuration.initial_conditions.kind = kind;
    configuration.initial_conditions.count = count;
    configuration.initial_conditions.total_mass = 3;
    return configuration;
}

} // namespace

TEST_CASE("a galaxy configuration is assembled with the mass and particles it asked for",
          "[sim][initial-conditions]") {
    // `count` and `total_mass` mean the same thing for a galaxy as they do for
    // the two spheres: the particles the run has and the mass they share. That
    // is the whole reason the configuration describes a galaxy through a bulge
    // fraction rather than through a disc mass and a bulge mass, and it is worth
    // a test because it is a property of the assembly rather than of the model.
    const Configuration configuration =
        galaxy_configuration(InitialConditionKind::kDiscGalaxy, 2000);
    const ParticleData data = make_initial_conditions(configuration);

    REQUIRE(data.size() == 2000);

    const Real mass = total_mass(data.masses());
    const Real requested = configuration.initial_conditions.total_mass;
    REQUIRE(std::abs(mass - requested) <
            static_cast<Real>(kSinglePrecision ? 1e-6 : 1e-9) * requested);
}

TEST_CASE("a collision divides the count and the mass between its two galaxies",
          "[sim][initial-conditions]") {
    Configuration configuration =
        galaxy_configuration(InitialConditionKind::kGalaxyCollision, 3000);
    configuration.initial_conditions.mass_ratio = static_cast<Real>(0.5);

    const ParticleData data = make_initial_conditions(configuration);

    REQUIRE(data.size() == 3000);

    const Real mass = total_mass(data.masses());
    const Real requested = configuration.initial_conditions.total_mass;
    REQUIRE(std::abs(mass - requested) <
            static_cast<Real>(kSinglePrecision ? 1e-6 : 1e-9) * requested);

    // Both galaxies are made of particles of one mass, which is what makes the
    // count a resolution rather than an arbitrary division. A split that gave
    // the secondary its share of the mass but not of the particles would fail
    // here and nowhere else.
    const std::span<const Real> masses = data.masses();
    for (Index particle = 0; particle < data.size(); ++particle) {
        REQUIRE(std::abs(masses[particle] - masses[0]) <
                static_cast<Real>(kSinglePrecision ? 1e-6 : 1e-12) * masses[0]);
    }
}

TEST_CASE("a collision is placed at the separation the configuration gives",
          "[sim][initial-conditions]") {
    Configuration configuration =
        galaxy_configuration(InitialConditionKind::kGalaxyCollision, 2000);
    configuration.initial_conditions.separation = 30;
    configuration.initial_conditions.impact_parameter = 4;

    const ParticleData data = make_initial_conditions(configuration);

    // Measured as the distance between the two extremes rather than between the
    // two centres, which the assembly does not report. Each galaxy is a few
    // scale lengths across and they are thirty apart, so a configuration that
    // had ignored the separation entirely would produce a cloud an order of
    // magnitude smaller than this.
    Real furthest = 0;
    for (Index particle = 0; particle < data.size(); ++particle) {
        furthest = std::max(furthest, norm(data.positions().get(particle)));
    }
    REQUIRE(furthest > 10);
}

TEST_CASE("a galaxy is built for the softening the run will use", "[sim][initial-conditions]") {
    // The one setting a galaxy takes from another section of the configuration.
    // A disc built for point masses and integrated with a softened kernel starts
    // out of balance, so the assembly passes the solver's softening through, and
    // the effect of it is that the disc rotates more slowly.
    Configuration configuration = galaxy_configuration(InitialConditionKind::kDiscGalaxy, 2000);
    const ParticleData sharp = make_initial_conditions(configuration);

    configuration.solver.softening = static_cast<Real>(0.2);
    const ParticleData softened = make_initial_conditions(configuration);

    // The same seed, so the two samples hold the same particles in the same
    // places and differ only in how fast they are going.
    REQUIRE(sharp.size() == softened.size());

    Real sharp_speed = 0;
    Real softened_speed = 0;
    for (Index particle = 0; particle < sharp.size(); ++particle) {
        sharp_speed += norm(sharp.velocities().get(particle));
        softened_speed += norm(softened.velocities().get(particle));
    }
    REQUIRE(softened_speed < sharp_speed);
}
