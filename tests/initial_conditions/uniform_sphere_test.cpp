#include "orrery/initial_conditions/uniform_sphere.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

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
using orrery::core::Diagnostics;
using orrery::core::Index;
using orrery::core::measure_diagnostics;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::total_mass;
using orrery::core::Vec3;
using orrery::initial_conditions::make_uniform_sphere;
using orrery::initial_conditions::uniform_sphere_potential_energy;
using orrery::initial_conditions::UniformSphereParameters;

constexpr std::uint64_t kSeed = 20260810;

constexpr Index kCount = 4096;

} // namespace

TEST_CASE("a sampled sphere has the potential energy of a uniform sphere",
          "[validation][initial-conditions]") {
    // The simplest analytic check available on the potential energy diagnostic.
    // A uniform sphere of mass M and radius R has potential energy -3GM^2/5R,
    // a result that needs no sampling theory and no equilibrium argument to
    // state, so a disagreement here is a disagreement about the diagnostic or
    // about the sampling of the volume and nothing else.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr UniformSphereParameters kModel{.count = kCount, .total_mass = 4, .radius = 3};
    const ParticleData data = make_uniform_sphere(kModel, random);
    const Diagnostics measured = measure_diagnostics(data, Softening{});

    const Real expected = uniform_sphere_potential_energy(kModel);

    // The scatter of the sampled energy falls as the reciprocal square root of
    // the count, and over two dozen seeds at this count the largest departure
    // was under one per cent. Five per cent is a bound a correct sample does
    // not approach, and one that a radius drawn uniformly instead of as a cube
    // root, which concentrates the mass and deepens the energy by tens of per
    // cent, could not satisfy.
    CAPTURE(measured.potential_energy, expected);
    REQUIRE(std::abs(measured.potential_energy - expected) <=
            static_cast<Real>(0.05) * std::abs(expected));
}

TEST_CASE("the sphere is uniform rather than centrally concentrated",
          "[validation][initial-conditions]") {
    // The moment test behind the energy test above, stated directly. Half the
    // radius encloses an eighth of the volume, and therefore an eighth of the
    // particles.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr UniformSphereParameters kModel{.count = kCount};
    const ParticleData data = make_uniform_sphere(kModel, random);

    Index inside = 0;
    for (Index particle = 0; particle < data.size(); ++particle) {
        if (norm(data.positions().get(particle)) < kModel.radius / 2) {
            ++inside;
        }
    }

    // A binomial count of this many draws at a probability of an eighth has a
    // standard deviation of 21, so five of those is a bound that will not be
    // crossed by chance and that a uniformly drawn radius, which would put a
    // quarter of the particles inside, misses by a factor of two.
    const auto expected = static_cast<Real>(kCount) / 8;
    CAPTURE(inside, expected);
    REQUIRE(std::abs(static_cast<Real>(inside) - expected) <= 105);
}

TEST_CASE("every particle is inside the sphere", "[property][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr UniformSphereParameters kModel{.count = kCount, .radius = 2};
    const ParticleData data = make_uniform_sphere(kModel, random);

    // The bound is the radius plus the recentring shift. That shift is the
    // sampled centre of mass, which for a uniform sphere sits at about 0.8
    // radii over the square root of the count; the factor below is margin on
    // top of it, and it still rejects a sampler that let a particle out of the
    // sphere at all.
    const Real bound = kModel.radius * (1 + (4 / std::sqrt(static_cast<Real>(kCount))));
    for (Index particle = 0; particle < data.size(); ++particle) {
        CAPTURE(particle);
        REQUIRE(norm(data.positions().get(particle)) <= bound);
    }
}

TEST_CASE("the sphere starts cold and centred", "[unit][initial-conditions]") {
    // Cold, meaning at rest: half the kinetic energy the virial theorem asks
    // for is what makes this the collapse configuration rather than an
    // equilibrium one, and none at all is the extreme case of that.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr UniformSphereParameters kModel{.count = 512};
    const ParticleData data = make_uniform_sphere(kModel, random);
    const Diagnostics measured = measure_diagnostics(data, Softening{});

    for (Index particle = 0; particle < data.size(); ++particle) {
        CAPTURE(particle);
        REQUIRE(data.velocities().get(particle) == Vec3{});
    }

    // Exactly zero rather than nearly: a sum of zeros loses nothing, and the
    // recentring subtracts a zero drift from zero velocities.
    REQUIRE(measured.kinetic_energy == Real{0});
    REQUIRE(measured.linear_momentum == Vec3{});
    REQUIRE(measured.angular_momentum == Vec3{});

    const Real tolerance = 64 * std::numeric_limits<Real>::epsilon() * kModel.radius;
    CAPTURE(norm(centre_of_mass(data.positions(), data.masses())));
    REQUIRE(norm(centre_of_mass(data.positions(), data.masses())) <= tolerance);

    // A cold configuration has no kinetic energy, so its virial ratio is zero
    // and it is as far from equilibrium as a bound system can be.
    REQUIRE(measured.virial_ratio() == Real{0});
}

TEST_CASE("a uniform sphere shares its mass equally", "[unit][initial-conditions]") {
    RandomSource random{kSeed};

    constexpr UniformSphereParameters kModel{.count = 8, .total_mass = 2};
    const ParticleData data = make_uniform_sphere(kModel, random);

    REQUIRE(total_mass(data.masses()) == Real{2});
    for (const Real mass : data.masses()) {
        REQUIRE(mass == static_cast<Real>(0.25));
    }
}

TEST_CASE("a sphere that cannot be sampled is rejected", "[unit][initial-conditions]") {
    RandomSource random{kSeed};

    REQUIRE(make_uniform_sphere({.count = 0}, random).empty());
    REQUIRE_THROWS_AS(make_uniform_sphere({.count = 10, .total_mass = 0}, random),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(make_uniform_sphere({.count = 10, .radius = -1}, random),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(uniform_sphere_potential_energy({.radius = 0}), std::invalid_argument);
}
