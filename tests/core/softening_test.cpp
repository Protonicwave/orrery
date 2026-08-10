#include "orrery/core/softening.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/types.hpp"

namespace {

using orrery::core::Real;
using orrery::core::softened_inverse_distance;
using orrery::core::softened_inverse_distance_cubed;
using orrery::core::Softening;

constexpr std::uint_fast32_t kSeed = 20260810;

/// The softened potential of a unit mass at a given separation, which is what
/// the diagnostic in `diagnostics.hpp` sums over pairs.
[[nodiscard]] Real potential(Real separation, Softening softening) {
    return -softened_inverse_distance(separation * separation, softening);
}

/// The magnitude of the softened acceleration a unit mass produces, which is
/// what the force kernel of Phase 5 will sum over pairs.
[[nodiscard]] Real acceleration(Real separation, Softening softening) {
    return separation * softened_inverse_distance_cubed(separation * separation, softening);
}

} // namespace

TEST_CASE("a default softening leaves the point-mass result alone", "[unit][core]") {
    constexpr Softening kNone;

    REQUIRE(kNone.squared() == Real{0});
    REQUIRE(kNone.length() == Real{0});

    // Separations whose reciprocals are exact in binary, so the comparison can
    // be exact and a failure means the formula is wrong rather than that a
    // tolerance was too tight.
    REQUIRE(softened_inverse_distance(Real{4}, kNone) == static_cast<Real>(0.5));
    REQUIRE(softened_inverse_distance(Real{16}, kNone) == static_cast<Real>(0.25));
    REQUIRE(softened_inverse_distance_cubed(Real{4}, kNone) == static_cast<Real>(0.125));
}

TEST_CASE("the softening length enters as its square", "[unit][core]") {
    constexpr Softening kSoftening{4};

    REQUIRE(kSoftening.squared() == Real{16});
    REQUIRE(kSoftening.length() == Real{4});

    // At zero separation the point-mass result is infinite and this one is not,
    // which is the entire purpose of the softening: the potential there is that
    // of a mass at one softening length.
    REQUIRE(softened_inverse_distance(Real{0}, kSoftening) == static_cast<Real>(0.25));

    // A squared separation of 48 with this softening gives a softened distance
    // of exactly 8, so both results are exact powers of two and the comparisons
    // below need no tolerance.
    REQUIRE(softened_inverse_distance(Real{48}, kSoftening) == static_cast<Real>(0.125));
    REQUIRE(softened_inverse_distance_cubed(Real{48}, kSoftening) ==
            static_cast<Real>(0.001953125));
}

TEST_CASE("a negative softening length is the same as its magnitude", "[unit][core]") {
    // Only the square is kept, so the sign cannot matter. Stated as a test
    // rather than left implied, because the constructor accepts a value it does
    // not check and a reader is entitled to know what happens then.
    constexpr Softening kNegative{-3};
    constexpr Softening kPositive{3};

    REQUIRE(kNegative.squared() == kPositive.squared());
}

TEST_CASE("the acceleration is the gradient of the potential the diagnostic uses",
          "[property][core]") {
    // The load-bearing test of this file. The energy conservation results of
    // this project compare a potential energy against the work done by an
    // acceleration, and that comparison means nothing unless the two come from
    // the same potential. Here the second is checked to be the derivative of
    // the first by finite difference, over a range of separations either side
    // of the softening length, which is where the two forms differ most.
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};
    std::uniform_real_distribution<Real> choose_separation{static_cast<Real>(0.05), Real{20}};
    std::uniform_real_distribution<Real> choose_softening{Real{0}, Real{3}};

    // A central difference has two error terms that pull in opposite
    // directions: the truncation error grows as the square of the step and the
    // rounding error falls as its reciprocal. The cube root of the machine
    // epsilon balances them, and it follows the precision the build was
    // configured with rather than assuming double.
    const Real relative_step = std::cbrt(std::numeric_limits<Real>::epsilon());

    constexpr int kSamples = 500;
    for (int trial = 0; trial < kSamples; ++trial) {
        CAPTURE(trial);
        const Real separation = choose_separation(generator);
        const Softening softening{choose_softening(generator)};

        // The step follows the larger of the two lengths in the problem. Inside
        // the softening radius the potential varies on the scale of the
        // softening rather than of the separation, and a step chosen from the
        // separation alone would be far too small there, leaving the difference
        // below dominated by the rounding of two nearly equal potentials.
        const Real scale = std::max(separation, softening.length());
        const Real step = relative_step * scale;

        const Real derivative =
            (potential(separation + step, softening) - potential(separation - step, softening)) /
            (2 * step);

        // The acceleration points inward and the potential rises outward, so
        // the two are equal rather than opposite in sign once the magnitude is
        // taken.
        const Real expected = acceleration(separation, softening);

        // The tolerance is the sum of the two error terms rather than a
        // multiple of the result, because near the centre of a softened
        // potential the result goes to zero while neither error term does.
        // Rounding: two potentials of size 1/scale, each correct to a rounding,
        // divided by the step. Truncation: the third derivative, of order
        // 1/scale^4, times the square of the step.
        const Real rounding = std::numeric_limits<Real>::epsilon() / (step * scale);
        const Real truncation = (step * step) / (scale * scale * scale * scale);
        const Real tolerance = 32 * (rounding + truncation);

        CAPTURE(separation, softening.length(), derivative, expected, tolerance);
        REQUIRE(std::abs(derivative - expected) <= tolerance);
    }
}

TEST_CASE("the cubed form is the cube of the plain one", "[property][core]") {
    // Not a restatement of the implementation: it is the guarantee that the
    // acceleration and the potential energy are built from bit-identical values
    // of the softened distance, which is what stops a spurious energy drift of
    // the size of a rounding per interaction.
    INFO("seed = " << kSeed);
    std::mt19937 generator{kSeed};
    std::uniform_real_distribution<Real> choose_squared{Real{0}, Real{400}};
    std::uniform_real_distribution<Real> choose_softening{Real{0}, Real{3}};

    constexpr int kSamples = 500;
    for (int trial = 0; trial < kSamples; ++trial) {
        CAPTURE(trial);
        const Real separation_squared = choose_squared(generator);
        const Softening softening{choose_softening(generator)};

        const Real inverse = softened_inverse_distance(separation_squared, softening);
        REQUIRE(softened_inverse_distance_cubed(separation_squared, softening) ==
                inverse * inverse * inverse);
    }
}
