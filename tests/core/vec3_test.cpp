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
        // magnitudes that cancelled to produce it rather than from the result.
        // Each component of the cross product carries a rounding error of order
        // epsilon |a| |b|, and dotting it with a vector multiplies that error
        // by the length of that vector. The two checks therefore have different
        // scales, and using one for both leaves the looser check sitting on its
        // boundary whenever the two vectors differ much in length. The factor
        // covers the handful of operations involved, and the tolerances hold in
        // both precisions because epsilon follows the build.
        const Real factor = Real{32} * std::numeric_limits<Real>::epsilon();

        REQUIRE(std::abs(dot(perpendicular, a)) <= factor * squared_norm(a) * norm(b));
        REQUIRE(std::abs(dot(perpendicular, b)) <= factor * norm(a) * squared_norm(b));
    }
}

TEST_CASE("the products obey their symmetries", "[property][core]") {
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};

    for (int trial = 0; trial < kSamples; ++trial) {
        CAPTURE(trial);
        const Vec3 a = sample(generator);
        const Vec3 b = sample(generator);

        // The dot product's symmetry is exact, and deliberately checked that
        // way. Swapping the arguments swaps the operands of each
        // multiplication, which does not change a rounded product, and leaves
        // the order of the additions alone, so anything other than equality
        // means the terms are not the ones the definition calls for.
        REQUIRE(dot(a, b) == dot(b, a));

        // The cross product's antisymmetry is exact in arithmetic and not in
        // evaluation. Each component is a difference of two products, and a
        // compiler is permitted to contract a multiplication and an addition
        // into a single fused operation, which it does by default on hardware
        // that has the instruction. The fused product keeps its full precision
        // while the other is rounded, and swapping the arguments swaps which of
        // the two that is, so the two results can differ in the last bit. This
        // is not hypothetical: the exact form of this check passed on x86-64,
        // whose baseline has no fused multiply-add, and failed on Apple
        // silicon, which does.
        //
        // The tolerance is therefore the size of one rounding of a product of
        // magnitude |a| |b|, with a factor of four for the operations around
        // it.
        const Vec3 forward = cross(a, b);
        const Vec3 reversed = -cross(b, a);
        const Real tolerance = Real{4} * std::numeric_limits<Real>::epsilon() * norm(a) * norm(b);

        REQUIRE(std::abs(forward.x - reversed.x) <= tolerance);
        REQUIRE(std::abs(forward.y - reversed.y) <= tolerance);
        REQUIRE(std::abs(forward.z - reversed.z) <= tolerance);
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
