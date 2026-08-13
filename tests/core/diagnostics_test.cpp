#include "orrery/core/diagnostics.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"

namespace {

using orrery::core::angular_momentum;
using orrery::core::centre_of_mass;
using orrery::core::centre_of_mass_velocity;
using orrery::core::Diagnostics;
using orrery::core::Index;
using orrery::core::kGravitationalConstant;
using orrery::core::kinetic_energy;
using orrery::core::linear_momentum;
using orrery::core::measure_diagnostics;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::potential_energy;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::relative_energy_error;
using orrery::core::Softening;
using orrery::core::total_mass;
using orrery::core::Vec3;

constexpr std::uint64_t kSeed = 20260810;

/// A value whose last bit is far above one, in either precision.
///
/// The compensated summation test below needs a term large enough that adding
/// one to it changes nothing. In double precision the gap between neighbouring
/// values here is 2, and in single precision it is about a billion, so the
/// value works for both and the test asserts the same thing in each build.
constexpr Real kLarge = static_cast<Real>(1e16);

/// Two unit masses two units apart on the x axis, moving in opposite directions
/// along y at unit speed.
///
/// Every quantity of this configuration is a small exact binary number, so the
/// tests below can compare exactly and a failure means the formula is wrong
/// rather than that a tolerance was chosen badly.
[[nodiscard]] ParticleData two_body() {
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{0, -1, 0}, Real{1});
    data.add(Vec3{1, 0, 0}, Vec3{0, 1, 0}, Real{1});

    return data;
}

/// A configuration of random particles, for the invariance properties below.
[[nodiscard]] ParticleData scattered(Index count, RandomSource& random) {
    ParticleData data;
    data.reserve(count);

    for (Index particle = 0; particle < count; ++particle) {
        data.add(3 * random.unit_vector(), random.unit_vector(),
                 random.uniform(static_cast<Real>(0.5), 2));
    }

    return data;
}

} // namespace

TEST_CASE("an empty configuration has nothing to report", "[unit][core]") {
    const ParticleData data;
    const Diagnostics measured = measure_diagnostics(data, Softening{});

    REQUIRE(measured.kinetic_energy == Real{0});
    REQUIRE(measured.potential_energy == Real{0});
    REQUIRE(measured.linear_momentum == Vec3{});
    REQUIRE(measured.angular_momentum == Vec3{});

    // The centre of a configuration with no mass in it is not a number that
    // exists. The origin is returned rather than a quotient of zeros, so that a
    // sampler recentring an empty container leaves it empty rather than filling
    // it with NaNs.
    REQUIRE(total_mass(data.masses()) == Real{0});
    REQUIRE(centre_of_mass(data.positions(), data.masses()) == Vec3{});
    REQUIRE(centre_of_mass_velocity(data.velocities(), data.masses()) == Vec3{});
}

TEST_CASE("each quantity of the two-body configuration is its hand-computed value",
          "[unit][core]") {
    const ParticleData data = two_body();

    REQUIRE(total_mass(data.masses()) == Real{2});
    REQUIRE(centre_of_mass(data.positions(), data.masses()) == Vec3{});
    REQUIRE(centre_of_mass_velocity(data.velocities(), data.masses()) == Vec3{});

    // Two unit masses at unit speed.
    REQUIRE(kinetic_energy(data.velocities(), data.masses()) == Real{1});

    // One pair, unit masses, separation two, so -G/2 with G one.
    REQUIRE(potential_energy(data.positions(), data.masses(), Softening{}) ==
            static_cast<Real>(-0.5));

    // Equal and opposite velocities.
    REQUIRE(linear_momentum(data.velocities(), data.masses()) == Vec3{});

    // Each particle contributes a unit displacement crossed with a unit
    // velocity, both in the x-y plane and both in the same rotational sense.
    REQUIRE(angular_momentum(data.positions(), data.velocities(), data.masses()) == Vec3{0, 0, 2});
}

TEST_CASE("the derived quantities follow the measured ones", "[unit][core]") {
    const Diagnostics measured{
        .kinetic_energy = 1, .potential_energy = -4, .linear_momentum = {}, .angular_momentum = {}};

    REQUIRE(measured.total_energy() == Real{-3});

    // Half the kinetic energy the virial theorem would ask for, which is the
    // state the cold uniform sphere starts in.
    REQUIRE(measured.virial_ratio() == static_cast<Real>(0.5));
}

TEST_CASE("the energy error is relative to where the run started", "[unit][core]") {
    // An eighth lost from an energy of −4, which is the sign convention every
    // energy plot in the project is drawn with: a system that has shed energy
    // reports a negative error. Every value here is exact in binary, so a
    // failure means the formula is wrong rather than that a tolerance was
    // chosen badly.
    REQUIRE(relative_energy_error(Real{-4}, static_cast<Real>(-4.5)) == static_cast<Real>(-0.125));

    REQUIRE(relative_energy_error(Real{-4}, Real{-4}) == Real{0});

    // A run whose first measured energy is zero has no scale to be relative to,
    // and the absolute difference is the answer rather than an infinity.
    REQUIRE(relative_energy_error(Real{0}, static_cast<Real>(0.25)) == static_cast<Real>(0.25));
}

TEST_CASE("softening enters the potential energy as it enters the force", "[unit][core]") {
    // Two unit masses at the origin. Unsoftened their potential energy is
    // infinite; with a softening of one it is the potential of two masses one
    // softening length apart, which is the property that makes the softened
    // system a real one rather than a numerical convenience.
    ParticleData data;
    data.add(Vec3{}, Vec3{}, Real{1});
    data.add(Vec3{}, Vec3{}, Real{1});

    REQUIRE(potential_energy(data.positions(), data.masses(), Softening{1}) ==
            -kGravitationalConstant);

    // The same pair with the softening at 4 and a separation of sqrt(48), where
    // the softened distance is exactly 8.
    ParticleData separated;
    separated.add(Vec3{0, 0, 0}, Vec3{}, Real{1});
    separated.add(Vec3{4, 4, 4}, Vec3{}, Real{1});

    REQUIRE(potential_energy(separated.positions(), separated.masses(), Softening{4}) ==
            static_cast<Real>(-0.125));
}

TEST_CASE("the momentum sum keeps the digits an ordinary total would lose", "[unit][core]") {
    // A sum whose exact value is representable but whose partial sums are not.
    // Accumulated left to right without compensation, the ten unit velocities
    // vanish into a running total of 1e16 and the answer comes back as zero.
    // This is not a contrived worry: the momentum of a cluster is exactly this
    // shape, a small residue between large opposing contributions, and a
    // conservation test that cannot resolve the residue is not measuring
    // anything.
    ParticleData data;
    data.add(Vec3{}, Vec3{kLarge, 0, 0}, Real{1});

    constexpr Index kSmallCount = 10;
    for (Index particle = 0; particle < kSmallCount; ++particle) {
        data.add(Vec3{}, Vec3{1, 0, 0}, Real{1});
    }

    data.add(Vec3{}, Vec3{-kLarge, 0, 0}, Real{1});

    REQUIRE(linear_momentum(data.velocities(), data.masses()).x == static_cast<Real>(kSmallCount));
}

TEST_CASE("the potential energy does not depend on where the system is", "[property][core]") {
    // Gravity depends on separations alone, so a rigid translation cannot
    // change the potential energy. This is the property that lets the samplers
    // recentre their output without disturbing its physics, and it holds only
    // to round-off because the shifted coordinates are rounded.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr Softening kSoftening{static_cast<Real>(0.1)};

    const ParticleData data = scattered(200, random);
    const Real reference = potential_energy(data.positions(), data.masses(), kSoftening);

    for (int trial = 0; trial < 20; ++trial) {
        CAPTURE(trial);
        const Vec3 shift = 50 * random.unit_vector();

        ParticleData moved = data;
        const auto positions = moved.positions();
        for (Index particle = 0; particle < moved.size(); ++particle) {
            positions.set(particle, positions.get(particle) + shift);
        }

        const Real shifted = potential_energy(moved.positions(), moved.masses(), kSoftening);

        // Each separation is a difference of two shifted coordinates, so it
        // carries an absolute error of about the machine epsilon times the
        // shift. Its relative error is that divided by the separation, and the
        // softening length bounds the softened separation from below, so the
        // worst relative error any pair can contribute is the ratio below. The
        // remaining factor is margin for the number of pairs.
        const Real tolerance = 64 * std::numeric_limits<Real>::epsilon() * std::abs(reference) *
                               norm(shift) / kSoftening.length();
        CAPTURE(reference, shifted, tolerance);
        REQUIRE(std::abs(shifted - reference) <= tolerance);
    }
}

TEST_CASE("the potential energy scales as the reciprocal of the size", "[property][core]") {
    // Doubling every separation halves the potential energy of an unsoftened
    // system. Together with the translation property above this pins the
    // functional form: the two together are only satisfied by a sum over pairs
    // of the reciprocal separation.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = scattered(200, random);
    const Real reference = potential_energy(data.positions(), data.masses(), Softening{});

    for (int trial = 1; trial <= 8; ++trial) {
        CAPTURE(trial);
        const auto factor = static_cast<Real>(trial);

        ParticleData scaled = data;
        const auto positions = scaled.positions();
        for (Index particle = 0; particle < scaled.size(); ++particle) {
            positions.set(particle, positions.get(particle) * factor);
        }

        const Real measured = potential_energy(scaled.positions(), scaled.masses(), Softening{});
        const Real expected = reference / factor;

        const Real tolerance = 64 * std::numeric_limits<Real>::epsilon() * std::abs(expected);
        CAPTURE(measured, expected, tolerance);
        REQUIRE(std::abs(measured - expected) <= tolerance);
    }
}

TEST_CASE("the combined measurement agrees with the individual ones", "[property][core]") {
    // The single-pass measurement exists for the simulation loop and the
    // separate functions for the tests, which means there are two
    // implementations of each quantity and they could disagree. They are
    // required to agree exactly: both accumulate the same terms in the same
    // order with the same compensation, so anything other than equality is a
    // difference in what they compute rather than in how they round.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    const ParticleData data = scattered(500, random);
    constexpr Softening kSoftening{static_cast<Real>(0.05)};

    const Diagnostics measured = measure_diagnostics(data, kSoftening);

    REQUIRE(measured.kinetic_energy == kinetic_energy(data.velocities(), data.masses()));
    REQUIRE(measured.potential_energy ==
            potential_energy(data.positions(), data.masses(), kSoftening));
    REQUIRE(measured.linear_momentum == linear_momentum(data.velocities(), data.masses()));
    REQUIRE(measured.angular_momentum ==
            angular_momentum(data.positions(), data.velocities(), data.masses()));
}
