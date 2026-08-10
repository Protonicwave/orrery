#include "orrery/initial_conditions/centre_of_mass_frame.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/diagnostics.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace {

using orrery::core::centre_of_mass;
using orrery::core::centre_of_mass_velocity;
using orrery::core::Index;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::potential_energy;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::move_to_centre_of_mass_frame;

constexpr std::uint64_t kSeed = 20260810;

/// A configuration deliberately off centre and drifting, so that the shift the
/// function has to remove is far larger than the residue it should leave.
[[nodiscard]] ParticleData offset_cloud(Index count, RandomSource& random) {
    constexpr Vec3 kOffset{100, -50, 25};
    constexpr Vec3 kDrift{-3, 7, 2};

    ParticleData data;
    data.reserve(count);

    for (Index particle = 0; particle < count; ++particle) {
        data.add(kOffset + (2 * random.unit_vector()), kDrift + random.unit_vector(),
                 random.uniform(static_cast<Real>(0.5), 4));
    }

    return data;
}

} // namespace

TEST_CASE("the shifted configuration is centred and at rest", "[unit][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    ParticleData data = offset_cloud(500, random);
    move_to_centre_of_mass_frame(data);

    // The residue is what one rounding of a coordinate of order the original
    // offset leaves behind, which is a hundred times the epsilon rather than
    // the fifty the offset itself was.
    const Real position_tolerance = 64 * std::numeric_limits<Real>::epsilon() * 100;
    const Real velocity_tolerance = 64 * std::numeric_limits<Real>::epsilon() * 10;

    const Vec3 centre = centre_of_mass(data.positions(), data.masses());
    const Vec3 drift = centre_of_mass_velocity(data.velocities(), data.masses());

    CAPTURE(norm(centre), position_tolerance);
    REQUIRE(norm(centre) <= position_tolerance);

    CAPTURE(norm(drift), velocity_tolerance);
    REQUIRE(norm(drift) <= velocity_tolerance);
}

TEST_CASE("the shift preserves the relative configuration", "[property][initial-conditions]") {
    // A translation cannot change the physics, which is the reason a sampler is
    // allowed to apply one. Separations are compared rather than positions,
    // because the positions are exactly what the function is meant to change.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData original = offset_cloud(200, random);
    ParticleData shifted = original;
    move_to_centre_of_mass_frame(shifted);

    REQUIRE(shifted.size() == original.size());

    const Real tolerance = 64 * std::numeric_limits<Real>::epsilon() * 100;
    for (Index particle = 1; particle < original.size(); ++particle) {
        CAPTURE(particle);

        const Vec3 before = original.positions().get(particle) - original.positions().get(0);
        const Vec3 after = shifted.positions().get(particle) - shifted.positions().get(0);
        REQUIRE(norm(after - before) <= tolerance);

        const Vec3 before_velocity =
            original.velocities().get(particle) - original.velocities().get(0);
        const Vec3 after_velocity =
            shifted.velocities().get(particle) - shifted.velocities().get(0);
        REQUIRE(norm(after_velocity - before_velocity) <= tolerance);

        REQUIRE(shifted.masses()[particle] == original.masses()[particle]);
    }

    // The potential energy is the quantity that would notice if the separations
    // had moved, and it is the one a conservation test will later compare
    // against.
    const Real before = potential_energy(original.positions(), original.masses(),
                                         Softening{static_cast<Real>(0.1)});
    const Real after =
        potential_energy(shifted.positions(), shifted.masses(), Softening{static_cast<Real>(0.1)});

    CAPTURE(before, after);
    REQUIRE(std::abs(after - before) <=
            1024 * std::numeric_limits<Real>::epsilon() * std::abs(before));
}

TEST_CASE("shifting an empty configuration does nothing", "[unit][initial-conditions]") {
    // The centre of a massless configuration is not a number, and this is the
    // path that would propagate it if the diagnostics returned one.
    ParticleData data;
    move_to_centre_of_mass_frame(data);

    REQUIRE(data.empty());
}

TEST_CASE("shifting twice changes nothing the second time", "[property][initial-conditions]") {
    // The operation is idempotent to round-off, which is the check that it
    // removes what it measures rather than something close to it.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    ParticleData data = offset_cloud(300, random);
    move_to_centre_of_mass_frame(data);

    const ParticleData once = data;
    move_to_centre_of_mass_frame(data);

    const Real tolerance = 64 * std::numeric_limits<Real>::epsilon() * 100;
    for (Index particle = 0; particle < data.size(); ++particle) {
        CAPTURE(particle);
        REQUIRE(norm(data.positions().get(particle) - once.positions().get(particle)) <= tolerance);
        REQUIRE(norm(data.velocities().get(particle) - once.velocities().get(particle)) <=
                tolerance);
    }
}
