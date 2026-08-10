#include "orrery/core/vec3.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"

namespace {

using orrery::core::cross;
using orrery::core::dot;
using orrery::core::norm;
using orrery::core::Real;
using orrery::core::squared_norm;
using orrery::core::Vec3;

/// The seed for the property tests below. Fixed so that a failure can be
/// reproduced exactly, and reported in the failure message so that the reader
/// does not have to open this file to find it.
constexpr std::uint_fast32_t kSeed = 20260810;

constexpr int kSamples = 1000;

[[nodiscard]] Vec3 sample(std::mt19937& generator) {
    // Components spanning both signs, so that cancellation in the identities
    // below is exercised rather than avoided.
    std::uniform_real_distribution<Real> distribution{-1, 1};
    return {distribution(generator), distribution(generator), distribution(generator)};
}

// The arithmetic is constant-evaluated here as well as run below, because the
// analytic configurations of Phase 3 are written as constant expressions and a
// type that only works at run time would not support them.
static_assert(Vec3{} == Vec3{0, 0, 0});
static_assert(Vec3{1, 2, 3} + Vec3{4, 5, 6} == Vec3{5, 7, 9});
static_assert(Vec3{4, 5, 6} - Vec3{1, 2, 3} == Vec3{3, 3, 3});
static_assert(-Vec3{1, -2, 3} == Vec3{-1, 2, -3});
static_assert(Vec3{1, 2, 3} * 2 == Vec3{2, 4, 6});
static_assert(2 * Vec3{1, 2, 3} == Vec3{2, 4, 6});
static_assert(Vec3{2, 4, 6} / 2 == Vec3{1, 2, 3});
static_assert(dot(Vec3{1, 2, 3}, Vec3{4, 5, 6}) == 32);
static_assert(cross(Vec3{1, 0, 0}, Vec3{0, 1, 0}) == Vec3{0, 0, 1});
static_assert(squared_norm(Vec3{3, 4, 0}) == 25);

} // namespace

TEST_CASE("compound assignment matches the corresponding binary operator", "[unit][core]") {
    Vec3 accumulated{1, 2, 3};

    accumulated += Vec3{4, 5, 6};
    REQUIRE(accumulated == Vec3{5, 7, 9});

    accumulated -= Vec3{1, 1, 1};
    REQUIRE(accumulated == Vec3{4, 6, 8});

    accumulated *= 2;
    REQUIRE(accumulated == Vec3{8, 12, 16});

    accumulated /= 4;
    REQUIRE(accumulated == Vec3{2, 3, 4});
}

TEST_CASE("the length of a 3-4-5 triangle is exact", "[unit][core]") {
    // Chosen because the square root is exact in both precisions, so the
    // comparison can be exact and a failure means the formula is wrong rather
    // than that a tolerance was too tight.
    REQUIRE(norm(Vec3{3, 4, 0}) == Real{5});
    REQUIRE(norm(Vec3{0, 0, 0}) == Real{0});
}

TEST_CASE("the cross product is perpendicular to both of its arguments", "[property][core]") {
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};

    for (int trial = 0; trial < kSamples; ++trial) {
        CAPTURE(trial);
        const Vec3 a = sample(generator);
        const Vec3 b = sample(generator);
        const Vec3 perpendicular = cross(a, b);

        // The exact answer is zero, so the tolerance has to come from the
        // magnitudes that cancelled to produce it: the terms of the dot product
        // are of order |a|^2 |b|, and each carries a rounding error of order
        // epsilon. The factor of 16 covers the handful of operations involved
        // with room to spare, and holds in both precisions because epsilon
        // follows the build.
        const Real scale = squared_norm(a) * norm(b);
        const Real tolerance = Real{16} * std::numeric_limits<Real>::epsilon() * scale;

        REQUIRE(std::abs(dot(perpendicular, a)) <= tolerance);
        REQUIRE(std::abs(dot(perpendicular, b)) <= tolerance);
    }
}

TEST_CASE("the products obey their symmetries exactly", "[property][core]") {
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};

    for (int trial = 0; trial < kSamples; ++trial) {
        CAPTURE(trial);
        const Vec3 a = sample(generator);
        const Vec3 b = sample(generator);

        // Both comparisons are exact rather than approximate, and that is the
        // point of the test. Swapping the arguments swaps the operands of each
        // multiplication and negates each subtraction, neither of which changes
        // a rounded result, so anything other than equality here means the
        // terms are not the ones the definitions call for.
        REQUIRE(dot(a, b) == dot(b, a));
        REQUIRE(cross(a, b) == -cross(b, a));
    }
}

TEST_CASE("the squared length agrees with the length", "[property][core]") {
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};

    for (int trial = 0; trial < kSamples; ++trial) {
        CAPTURE(trial);
        const Vec3 v = sample(generator);
        const Real length = norm(v);

        const Real tolerance = Real{4} * std::numeric_limits<Real>::epsilon() * squared_norm(v);
        REQUIRE(std::abs((length * length) - squared_norm(v)) <= tolerance);
    }
}
