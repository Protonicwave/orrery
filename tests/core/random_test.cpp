#include "orrery/core/random.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"

namespace {

using orrery::core::norm;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Vec3;

constexpr std::uint64_t kSeed = 20260810;

constexpr int kSamples = 100000;

} // namespace

TEST_CASE("a stream reports the seed it was started from", "[unit][core]") {
    const RandomSource random{kSeed};

    REQUIRE(random.seed() == kSeed);
}

TEST_CASE("the same seed gives the same numbers", "[unit][core]") {
    // The property every reproducible result in this project rests on. Two
    // streams are compared rather than one stream against stored values,
    // because stored values would also assert which standard library the test
    // was built against, and the guarantee is about the seed rather than about
    // the platform.
    RandomSource first{kSeed};
    RandomSource second{kSeed};

    for (int draw = 0; draw < 1000; ++draw) {
        CAPTURE(draw);
        REQUIRE(first.uniform() == second.uniform());
        REQUIRE(first.unit_vector() == second.unit_vector());
    }
}

TEST_CASE("different seeds give different numbers", "[unit][core]") {
    RandomSource first{kSeed};
    RandomSource second{kSeed + 1};

    // Two independent streams agreeing on a single draw is possible and would
    // not be a fault; agreeing on ten in succession would mean the seed is
    // being ignored.
    int agreements = 0;
    for (int draw = 0; draw < 10; ++draw) {
        if (first.uniform() == second.uniform()) {
            ++agreements;
        }
    }

    REQUIRE(agreements < 10);
}

TEST_CASE("every draw lies in the half-open unit interval", "[property][core]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    for (int draw = 0; draw < kSamples; ++draw) {
        const Real value = random.uniform();
        CAPTURE(draw, value);

        // The upper bound is the one that matters. Several samplers raise the
        // draw to a negative power or use it as a mass fraction, and a value of
        // exactly one would send the radius those produce to infinity.
        REQUIRE(value >= Real{0});
        REQUIRE(value < Real{1});
    }
}

TEST_CASE("a draw is an exact multiple of the mantissa's last bit", "[property][core]") {
    // The mapping from engine bits to a number is meant to be exact: the top
    // bits of one draw, scaled by a power of two. This checks that nothing
    // rounds, which is what guarantees the interval stays half open in the
    // single-precision build as well as the double-precision one.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const auto step = static_cast<Real>(std::uint64_t{1} << std::numeric_limits<Real>::digits);

    for (int draw = 0; draw < kSamples; ++draw) {
        const Real value = random.uniform();
        CAPTURE(draw, value);
        REQUIRE(value * step == std::floor(value * step));
    }
}

TEST_CASE("a bounded draw stays within its bounds", "[property][core]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    for (int draw = 0; draw < kSamples; ++draw) {
        const Real value = random.uniform(-3, 5);
        CAPTURE(draw, value);
        REQUIRE(value >= Real{-3});
        REQUIRE(value < Real{5});
    }
}

TEST_CASE("an empty range returns its endpoint", "[unit][core]") {
    // The interpolation the bounded draw is written as returns the endpoint
    // exactly when the two bounds coincide, whatever the fraction. Written as
    // a subtraction of the bounds it would return the endpoint plus a rounded
    // zero, which is the same number here and is not for large bounds.
    RandomSource random{kSeed};

    for (int draw = 0; draw < 100; ++draw) {
        CAPTURE(draw);
        REQUIRE(random.uniform(Real{7}, Real{7}) == Real{7});
    }
}

TEST_CASE("a random direction has unit length", "[property][core]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    // Three roundings separate the components from the identity that makes
    // them a unit vector, so the length is one to a few units in the last
    // place rather than exactly.
    const Real tolerance = 8 * std::numeric_limits<Real>::epsilon();

    for (int draw = 0; draw < kSamples; ++draw) {
        const Vec3 direction = random.unit_vector();
        CAPTURE(draw, direction.x, direction.y, direction.z);
        REQUIRE(std::abs(norm(direction) - Real{1}) <= tolerance);
    }
}

TEST_CASE("random directions cover the sphere evenly", "[property][core]") {
    // The wrong construction, an angle drawn uniformly from each of two ranges,
    // passes the length test above and fails this one: it puts a quarter of its
    // points in the two polar caps that hold a seventh of the area. The mean of
    // each component and of each squared component are the two lowest moments
    // that distinguish them.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    Vec3 sum;
    Vec3 squared_sum;
    for (int draw = 0; draw < kSamples; ++draw) {
        const Vec3 direction = random.unit_vector();
        sum += direction;
        squared_sum +=
            Vec3{direction.x * direction.x, direction.y * direction.y, direction.z * direction.z};
    }

    const auto count = static_cast<Real>(kSamples);

    // The mean of each component is zero by symmetry, and its standard error
    // over this many draws is 1/sqrt(3N), about 0.0018. Five of those is a
    // bound the test will not cross by chance while remaining far below the
    // 0.5 that the polar-clustering construction would produce.
    const Real mean_tolerance = 5 / std::sqrt(3 * count);
    CAPTURE(sum.x / count, sum.y / count, sum.z / count, mean_tolerance);
    REQUIRE(std::abs(sum.x / count) <= mean_tolerance);
    REQUIRE(std::abs(sum.y / count) <= mean_tolerance);
    REQUIRE(std::abs(sum.z / count) <= mean_tolerance);

    // The mean of each squared component is a third, since the three sum to one
    // and no direction is preferred. This is the moment that catches
    // clustering towards an axis, which leaves the means above at zero.
    const Real squared_tolerance = 5 * std::sqrt(Real{4} / (45 * count));
    CAPTURE(squared_sum.x / count, squared_sum.y / count, squared_sum.z / count);
    REQUIRE(std::abs((squared_sum.x / count) - (Real{1} / 3)) <= squared_tolerance);
    REQUIRE(std::abs((squared_sum.y / count) - (Real{1} / 3)) <= squared_tolerance);
    REQUIRE(std::abs((squared_sum.z / count) - (Real{1} / 3)) <= squared_tolerance);
}
