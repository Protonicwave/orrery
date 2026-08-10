#include "orrery/initial_conditions/plummer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

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
using orrery::initial_conditions::kStandardPlummerRadius;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::plummer_kinetic_energy;
using orrery::initial_conditions::plummer_potential_energy;
using orrery::initial_conditions::plummer_total_energy;
using orrery::initial_conditions::PlummerParameters;

constexpr std::uint64_t kSeed = 20260810;

/// The particle count the statistical tests below sample at.
///
/// Large enough that the scatter of a sampled energy is between one and two
/// per cent rather than several, and small enough that the N^2 potential energy
/// stays well inside a second even in an unoptimised build.
constexpr Index kCount = 4096;

/// The distances of every particle from the origin, sorted.
[[nodiscard]] std::vector<Real> sorted_radii(const ParticleData& data) {
    std::vector<Real> radii;
    radii.reserve(data.size());

    for (Index particle = 0; particle < data.size(); ++particle) {
        radii.push_back(norm(data.positions().get(particle)));
    }

    std::ranges::sort(radii);
    return radii;
}

} // namespace

TEST_CASE("the analytic energies satisfy the virial theorem", "[unit][initial-conditions]") {
    // A statement about the model rather than about a sample of it. In standard
    // N-body units the total energy of a unit-mass Plummer sphere is exactly
    // -1/4, which is the definition those units are chosen to satisfy.
    constexpr PlummerParameters kModel{.count = kCount};

    REQUIRE(kModel.scale_radius == kStandardPlummerRadius);

    const Real tolerance = 8 * std::numeric_limits<Real>::epsilon();
    CAPTURE(plummer_total_energy(kModel), plummer_potential_energy(kModel),
            plummer_kinetic_energy(kModel));

    REQUIRE(std::abs(plummer_total_energy(kModel) - static_cast<Real>(-0.25)) <= tolerance);
    REQUIRE(std::abs(plummer_potential_energy(kModel) - static_cast<Real>(-0.5)) <= tolerance);
    REQUIRE(std::abs(plummer_kinetic_energy(kModel) - static_cast<Real>(0.25)) <= tolerance);
}

TEST_CASE("a sampled sphere is in virial equilibrium", "[validation][initial-conditions]") {
    // The result the phase is defined by. A Plummer sphere is a self-consistent
    // equilibrium, so a correct sample of it has twice its kinetic energy equal
    // to the magnitude of its potential energy. Getting either the radial
    // profile or the velocity distribution wrong breaks this, and almost
    // nothing else the sampler could get wrong leaves it intact.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr PlummerParameters kModel{.count = kCount};
    const ParticleData data = make_plummer_sphere(kModel, random);
    const Diagnostics measured = measure_diagnostics(data, Softening{});

    // The tolerance is set by the sampling, not by the arithmetic. Both
    // energies are means over the sample and scatter by of order the reciprocal
    // square root of the count. Measured over two dozen seeds at this count,
    // the largest departure of the ratio from unity was 0.04, so ten per cent
    // is a bound a correct sampler does not approach while remaining far below
    // the 0.5 or the 1.0 that a wrong velocity distribution produces: sampling
    // speeds uniformly rather than from the distribution function lands the
    // ratio near 2, and omitting the escape speed scaling lands it near 0.
    constexpr Real kTolerance = static_cast<Real>(0.10);
    CAPTURE(measured.virial_ratio(), measured.kinetic_energy, measured.potential_energy);
    REQUIRE(std::abs(measured.virial_ratio() - Real{1}) <= kTolerance);
}

TEST_CASE("a sampled sphere has the energies of the model it came from",
          "[validation][initial-conditions]") {
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr PlummerParameters kModel{.count = kCount};
    const ParticleData data = make_plummer_sphere(kModel, random);
    const Diagnostics measured = measure_diagnostics(data, Softening{});

    // Each energy scatters about its continuum value by the same few per cent
    // as their ratio above, and for the same reason; the largest departure
    // measured over two dozen seeds at this count was 0.03 of the value. Two
    // systematic effects sit inside that and are far smaller: a sample of N
    // particles has one part in N fewer interacting pairs than the continuum,
    // and the mass fraction cutoff moves a thousandth of the mass inwards.
    constexpr Real kTolerance = static_cast<Real>(0.08);

    const Real expected_potential = plummer_potential_energy(kModel);
    CAPTURE(measured.potential_energy, expected_potential);
    REQUIRE(std::abs(measured.potential_energy - expected_potential) <=
            kTolerance * std::abs(expected_potential));

    const Real expected_kinetic = plummer_kinetic_energy(kModel);
    CAPTURE(measured.kinetic_energy, expected_kinetic);
    REQUIRE(std::abs(measured.kinetic_energy - expected_kinetic) <= kTolerance * expected_kinetic);
}

TEST_CASE("a sampled sphere is centred and at rest", "[validation][initial-conditions]") {
    // Not a property of the model, which is centred and at rest by definition,
    // but of the sampler, which has to remove the residue of the draw. Both
    // should come back at round-off rather than at the square root of N over N
    // that an uncorrected sample would show.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr PlummerParameters kModel{.count = kCount};
    const ParticleData data = make_plummer_sphere(kModel, random);

    const Real mass = total_mass(data.masses());
    REQUIRE(std::abs(mass - kModel.total_mass) <=
            64 * std::numeric_limits<Real>::epsilon() * kModel.total_mass);

    // The centre is the mean of coordinates spread over a few scale radii, so
    // the residue after subtracting it is that spread times a rounding, times
    // margin for the summation.
    const Real position_tolerance = 64 * std::numeric_limits<Real>::epsilon() * kModel.scale_radius;
    const Real centre = norm(centre_of_mass(data.positions(), data.masses()));
    CAPTURE(centre, position_tolerance);
    REQUIRE(centre <= position_tolerance);

    const Diagnostics measured = measure_diagnostics(data, Softening{});

    // Speeds in these units are of order the square root of the kinetic energy
    // per unit mass, so the momentum residue is scaled from that rather than
    // from one.
    const Real speed_scale = std::sqrt(2 * measured.kinetic_energy / mass);
    const Real momentum_tolerance = 64 * std::numeric_limits<Real>::epsilon() * mass * speed_scale;
    CAPTURE(norm(measured.linear_momentum), momentum_tolerance);
    REQUIRE(norm(measured.linear_momentum) <= momentum_tolerance);
}

TEST_CASE("the radial profile is the model's", "[validation][initial-conditions]") {
    // The virial ratio is a check on the velocities given the positions. This
    // is the check on the positions alone, and it uses the median rather than a
    // mean because the Plummer profile has a heavy enough tail that its mean
    // radius is dominated by the few outermost particles.
    //
    // Inverting the cumulative mass profile at a half gives a median radius of
    // a / sqrt(2^(2/3) - 1), which is 1.3048 scale radii.
    INFO("seed = " << kSeed);
    RandomSource random{kSeed};

    constexpr PlummerParameters kModel{.count = kCount};
    const ParticleData data = make_plummer_sphere(kModel, random);
    const std::vector<Real> radii = sorted_radii(data);

    const Real median = radii[radii.size() / 2];
    const Real expected = static_cast<Real>(1.3048) * kModel.scale_radius;

    // The median of a sample of this size scattered by up to three per cent of
    // itself over two dozen seeds, so eight per cent is the same kind of bound
    // as the virial tolerance above: comfortably clear of the sampling, and far
    // inside the tens of per cent that a mis-inverted profile would move it by.
    CAPTURE(median, expected);
    REQUIRE(std::abs(median - expected) <= static_cast<Real>(0.08) * expected);

    // The cutoff is what bounds the outermost particle. Without it the largest
    // radius of a sample this size would be some tens of scale radii and
    // occasionally far more; with it the enclosed mass fraction never exceeds
    // 0.999, whose radius is 38.7 scale radii.
    CAPTURE(radii.back());
    REQUIRE(radii.back() <= 40 * kModel.scale_radius);
}

TEST_CASE("the same seed samples the same sphere", "[unit][initial-conditions]") {
    constexpr PlummerParameters kModel{.count = 256};

    RandomSource first{kSeed};
    RandomSource second{kSeed};

    const ParticleData one = make_plummer_sphere(kModel, first);
    const ParticleData other = make_plummer_sphere(kModel, second);

    REQUIRE(one.size() == other.size());
    for (Index particle = 0; particle < one.size(); ++particle) {
        CAPTURE(particle);
        REQUIRE(one.positions().get(particle) == other.positions().get(particle));
        REQUIRE(one.velocities().get(particle) == other.velocities().get(particle));
        REQUIRE(one.masses()[particle] == other.masses()[particle]);
    }
}

TEST_CASE("a Plummer sphere shares its mass equally", "[unit][initial-conditions]") {
    RandomSource random{kSeed};

    constexpr PlummerParameters kModel{.count = 100, .total_mass = 50};
    const ParticleData data = make_plummer_sphere(kModel, random);

    for (const Real mass : data.masses()) {
        REQUIRE(mass == static_cast<Real>(0.5));
    }
}

TEST_CASE("an empty sphere is empty rather than an error", "[unit][initial-conditions]") {
    // A caller sweeping a range of counts should not have to special-case the
    // first entry, and the mass per particle of an empty sphere is a division
    // this avoids rather than performs.
    RandomSource random{kSeed};

    const ParticleData data = make_plummer_sphere({.count = 0}, random);
    REQUIRE(data.empty());
}

TEST_CASE("a model that cannot be sampled is rejected", "[unit][initial-conditions]") {
    RandomSource random{kSeed};

    REQUIRE_THROWS_AS(make_plummer_sphere({.count = 10, .total_mass = 0}, random),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(make_plummer_sphere({.count = 10, .scale_radius = -1}, random),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(make_plummer_sphere({.count = 10, .mass_fraction_cutoff = 1}, random),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(make_plummer_sphere({.count = 10, .mass_fraction_cutoff = 0}, random),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(plummer_potential_energy({.scale_radius = 0}), std::invalid_argument);
}
