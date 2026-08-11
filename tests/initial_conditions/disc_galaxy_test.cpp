#include "orrery/initial_conditions/disc_galaxy.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>
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

namespace {

using orrery::core::angular_momentum;
using orrery::core::centre_of_mass;
using orrery::core::cross;
using orrery::core::dot;
using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::linear_momentum;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::total_mass;
using orrery::core::Vec3;
using orrery::core::Vec3Span;
using orrery::initial_conditions::disc_galaxy_circular_speed;
using orrery::initial_conditions::disc_galaxy_disc_count;
using orrery::initial_conditions::disc_galaxy_enclosed_mass;
using orrery::initial_conditions::disc_galaxy_particle_mass;
using orrery::initial_conditions::disc_galaxy_spin_axis;
using orrery::initial_conditions::disc_galaxy_total_mass;
using orrery::initial_conditions::DiscGalaxyParameters;
using orrery::initial_conditions::make_disc_galaxy;

constexpr std::uint64_t kSeed = 20260811;

constexpr DiscGalaxyParameters kGalaxy{.count = 8192,
                                       .disc_mass = 1,
                                       .bulge_mass = static_cast<Real>(0.25),
                                       .scale_length = 1,
                                       .scale_height = static_cast<Real>(0.1),
                                       .bulge_radius = static_cast<Real>(0.2)};

/// The cylindrical radius about the galaxy's spin axis.
[[nodiscard]] Real cylindrical_radius(Vec3 position, Vec3 axis) {
    const Vec3 along = axis * dot(position, axis);
    return norm(position - along);
}

} // namespace

TEST_CASE("a sampled galaxy has the requested particles, all of one mass",
          "[unit][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_disc_galaxy(kGalaxy, random);
    REQUIRE(data.size() == kGalaxy.count);

    // Equal masses are what makes the split between disc and bulge a rounding of
    // a particle count rather than a free choice, and what makes a rendered
    // point of light proportional to mass. A single unequal particle would break
    // both, so every one is checked rather than the total alone.
    const std::span<const Real> masses = data.masses();
    const Real expected = disc_galaxy_particle_mass(kGalaxy);
    for (Index particle = 0; particle < data.size(); ++particle) {
        REQUIRE(masses[particle] == expected);
    }

    // The realised total is the particle mass times the count, which is the
    // requested total up to the rounding of the component split.
    const Real total = total_mass(masses);
    const Real requested = kGalaxy.disc_mass + kGalaxy.bulge_mass;
    REQUIRE(std::abs(total - requested) < static_cast<Real>(1e-6) * requested);
    REQUIRE(std::abs(disc_galaxy_total_mass(kGalaxy) - total) <
            static_cast<Real>(1e-6) * requested);
}

TEST_CASE("a sampled galaxy is at rest at the origin", "[property][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_disc_galaxy(kGalaxy, random);

    // The recentring is a subtraction of a rounded mean from every particle, so
    // what is left is round-off accumulated over the count rather than zero. The
    // bound is generous against that and tight against a galaxy that had not
    // been recentred at all, whose centre would sit a few hundredths of a scale
    // length from the origin and whose momentum would be of order the rotation
    // speed divided by the square root of the count.
    // The bound follows the precision the build was configured with, since what
    // is left after the recentring is round-off accumulated over the count and
    // a float carries nine fewer digits of it than a double.
    constexpr Real kResidue = static_cast<Real>(kSinglePrecision ? 1e-6 : 1e-9);

    REQUIRE(norm(centre_of_mass(data.positions(), data.masses())) < kResidue);
    REQUIRE(norm(linear_momentum(data.velocities(), data.masses())) < kResidue);
}

TEST_CASE("every disc particle is on the circular orbit its radius supports",
          "[validation][initial-conditions]") {
    // The disc is built by putting each particle at the speed the enclosed mass
    // at its radius supports, and this is the statement of that. It is checked
    // against the model's own circular speed function rather than against a
    // second formula here, because the function is the interface: a renderer or
    // a study that wants to know what the disc was built to do asks it, and if
    // it disagreed with the sample the answer would be wrong for everyone.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_disc_galaxy(kGalaxy, random);
    const Vec3 axis = disc_galaxy_spin_axis(kGalaxy);
    const Vec3Span<const Real> positions = data.positions();
    const Vec3Span<const Real> velocities = data.velocities();

    // The sample was boosted into its own centre-of-momentum frame after the
    // velocities were assigned, so every one of them carries the same offset. It
    // is a Galilean boost and so changes no physics, but it does mean the speeds
    // are not the circular speeds and a test comparing them directly would have
    // to be written to a tolerance of a per cent, which is loose enough to hide
    // a real mistake.
    //
    // The offset is recovered from one particle and then held to account for
    // every other. That turns a comparison against a tolerance chosen to cover
    // the sampling into a comparison against round-off.
    const auto circular_velocity = [&axis](Vec3 position) {
        const Vec3 offset = position - (axis * dot(position, axis));
        const Real radius = norm(offset);
        return cross(axis, offset) / radius * disc_galaxy_circular_speed(kGalaxy, radius);
    };

    const Vec3 boost = circular_velocity(positions.get(0)) - velocities.get(0);

    // A rotation and a cross product cost several units in the last place, and
    // the innermost disc particles are at a hundredth of a scale length where
    // the cancellation is worst, so the single-precision bound is far looser.
    // Both are orders of magnitude sharper than the per cent the boost itself
    // would produce, which is what makes this a test rather than a formality.
    constexpr Real kTolerance = static_cast<Real>(kSinglePrecision ? 5e-4 : 1e-5);

    for (Index particle = 1; particle < disc_galaxy_disc_count(kGalaxy); ++particle) {
        const Vec3 expected = circular_velocity(positions.get(particle));
        const Vec3 measured = velocities.get(particle) + boost;

        INFO("particle " << particle << " at radius "
                         << cylindrical_radius(positions.get(particle), axis));
        REQUIRE(norm(measured - expected) < kTolerance * norm(expected));

        // Circular means perpendicular to the axis as well as to the radius,
        // which the magnitude alone does not say. A velocity tipped out of the
        // plane would pass a comparison of speeds and put the particle on an
        // orbit that crossed the disc rather than lying in it.
        REQUIRE(std::abs(dot(expected, axis)) < kTolerance * norm(expected));
    }
}

TEST_CASE("the enclosed mass function describes the sampled galaxy",
          "[validation][initial-conditions]") {
    // The circular speeds above are self-consistent: they check that the sampler
    // used the function. This checks the other half, that the function describes
    // where the mass actually went, by counting it.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = make_disc_galaxy(kGalaxy, random);
    const Vec3 axis = disc_galaxy_spin_axis(kGalaxy);
    const Vec3Span<const Real> positions = data.positions();
    const std::span<const Real> masses = data.masses();

    for (const Real radius : {static_cast<Real>(0.5), static_cast<Real>(1), static_cast<Real>(2),
                              static_cast<Real>(4)}) {
        Real counted = 0;
        for (Index particle = 0; particle < data.size(); ++particle) {
            if (cylindrical_radius(positions.get(particle), axis) < radius) {
                counted += masses[particle];
            }
        }

        // Two effects put the count above the formula rather than on it, and
        // both are of the same sign. The bulge term is a spherical enclosed mass
        // while the count is over a cylinder, so bulge particles beyond the
        // radius but near the plane of the disc are counted and not predicted.
        // The sampling itself scatters by the square root of the number counted,
        // which at this count is under two per cent. Ten per cent covers both
        // without covering a formula that had the profile wrong.
        const Real predicted = disc_galaxy_enclosed_mass(kGalaxy, radius);
        INFO("radius = " << radius << ", counted " << counted << ", predicted " << predicted);
        REQUIRE(std::abs(counted - predicted) < static_cast<Real>(0.1) * predicted);
    }
}

TEST_CASE("a galaxy's angular momentum lies along its spin axis",
          "[validation][initial-conditions]") {
    // The sharpest available statement that the orientation parameters do what
    // they say. The bulge is isotropic and contributes nothing on average, the
    // disc contributes all of it, and the direction is a property of the
    // rotation rather than of any one particle.
    INFO("seed = " << kSeed);

    for (const Real inclination :
         {static_cast<Real>(0), std::numbers::pi_v<Real> / 3, std::numbers::pi_v<Real>}) {
        DiscGalaxyParameters parameters = kGalaxy;
        parameters.inclination = inclination;
        parameters.position_angle = static_cast<Real>(0.7);

        RandomSource random{kSeed};
        const ParticleData data = make_disc_galaxy(parameters, random);

        const Vec3 measured = angular_momentum(data.positions(), data.velocities(), data.masses());
        const Vec3 axis = disc_galaxy_spin_axis(parameters);

        // The component along the axis should be the whole of it. What is left
        // over is the bulge's random contribution, which falls as the square
        // root of its particle count and is a few parts in a thousand here.
        const Real along = dot(measured, axis);
        const Real total = norm(measured);
        INFO("inclination = " << inclination << ", |L| = " << total << ", L along axis " << along);
        REQUIRE(along > static_cast<Real>(0.99) * total);
    }
}

TEST_CASE("a galaxy with no bulge is all disc", "[unit][initial-conditions]") {
    DiscGalaxyParameters parameters = kGalaxy;
    parameters.bulge_mass = 0;

    REQUIRE(disc_galaxy_disc_count(parameters) == parameters.count);

    RandomSource random{kSeed};
    const ParticleData data = make_disc_galaxy(parameters, random);
    REQUIRE(data.size() == parameters.count);
}

TEST_CASE("softening lowers the circular speed it was built for", "[unit][initial-conditions]") {
    // A softened force is weaker than a point-mass force at every radius, so the
    // orbit it supports is slower. The effect is largest where the softening is
    // comparable to the radius, which is the region a disc built for the wrong
    // force law would tear itself apart in first.
    DiscGalaxyParameters softened = kGalaxy;
    softened.softening = static_cast<Real>(0.05);

    for (const Real radius : {static_cast<Real>(0.05), static_cast<Real>(1)}) {
        REQUIRE(disc_galaxy_circular_speed(softened, radius) <
                disc_galaxy_circular_speed(kGalaxy, radius));
    }

    // Far outside the softening length the two agree, which is the other half of
    // the same statement: softening is a change to the force at short range and
    // nowhere else.
    const Real far = 8;
    REQUIRE(std::abs(disc_galaxy_circular_speed(softened, far) -
                     disc_galaxy_circular_speed(kGalaxy, far)) <
            static_cast<Real>(1e-4) * disc_galaxy_circular_speed(kGalaxy, far));
}

TEST_CASE("a galaxy refuses parameters it cannot sample", "[unit][initial-conditions]") {
    RandomSource random{kSeed};

    const auto refuses = [&random](const DiscGalaxyParameters& parameters) {
        REQUIRE_THROWS_AS(make_disc_galaxy(parameters, random), std::invalid_argument);
    };

    DiscGalaxyParameters parameters = kGalaxy;
    parameters.disc_mass = 0;
    refuses(parameters);

    parameters = kGalaxy;
    parameters.bulge_mass = -1;
    refuses(parameters);

    parameters = kGalaxy;
    parameters.scale_length = 0;
    refuses(parameters);

    parameters = kGalaxy;
    parameters.scale_height = 0;
    refuses(parameters);

    parameters = kGalaxy;
    parameters.bulge_radius = 0;
    refuses(parameters);

    parameters = kGalaxy;
    parameters.softening = -1;
    refuses(parameters);

    parameters = kGalaxy;
    parameters.mass_fraction_cutoff = 1;
    refuses(parameters);
}
