#include <array>
#include <cstdint>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/force_solver.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kSinglePrecision;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Softening;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::DirectSolver;
using orrery::solvers::ForceSolver;

/// The seeds these properties are asserted over.
///
/// Several rather than one, because a property that holds for a single
/// configuration is an example. Fixed rather than drawn, and reported in every
/// failure message, because a property test whose failing input cannot be
/// reproduced is not evidence of anything.
constexpr std::array<std::uint64_t, 3> kSeeds{20260810, 4242, 987654321};

constexpr Index kParticles = 128;

/// A softening length a twentieth of the Plummer scale radius.
///
/// The properties below hold at any softening, including none. A softening is
/// used anyway so that a chance close pair among a hundred and twenty-eight
/// sampled particles cannot produce one acceleration many orders of magnitude
/// larger than the rest, which would leave the sum being tested dominated by a
/// single term and its round-off with it. The test would still pass; it would
/// simply have stopped measuring the whole configuration.
const Softening kSofteningLength{static_cast<Real>(0.03)};

/// The scale a round-off level residual is measured against.
///
/// A sum of terms that cancel cannot be compared against zero on its own, since
/// the answer is zero and the question is how much of the input survived the
/// cancellation. The magnitudes of the terms are what set that scale.
///
/// The measured residual is 5e-17 of that scale in double precision and 3.1e-8 in
/// single, in both cases below the resolution of one operation in the arithmetic
/// the sum is made of. The bounds are left several orders above those
/// measurements: the claim is that nothing survives the cancellation but
/// round-off, and pinning the bound to this machine's last digits would turn a
/// statement about the kernel into a statement about how a particular compiler
/// contracted a multiply and an add.
constexpr Real kResidualTolerance =
    kSinglePrecision ? static_cast<Real>(1e-5) : static_cast<Real>(1e-13);

/// The looser bound the translated comparison is held to.
///
/// Translation invariance is exact in arithmetic and only approximate in
/// floating point, and the loss is not the kernel's. A configuration a few scale
/// radii across, moved ten units from the origin, has coordinates around twenty
/// times larger than the separations computed from them, so every subtraction in
/// the kernel throws away more than four bits before the physics begins. The
/// measured departure is 1.4e-5 in single precision and below 1e-13 in double,
/// and the bounds follow those rather than the round-off floor of a sum whose
/// terms were never shifted.
constexpr Real kTranslationTolerance =
    kSinglePrecision ? static_cast<Real>(1e-4) : static_cast<Real>(1e-12);

/// A Plummer sphere, whose particles all carry the same mass.
[[nodiscard]] ParticleData sampled_sphere(RandomSource& random) {
    return make_plummer_sphere(PlummerParameters{.count = kParticles}, random);
}

/// Particles scattered through a ball, with masses spread over a factor of four.
///
/// The Plummer sampler gives every particle the same mass, so on its own it
/// would not distinguish a kernel that had confused the mass of the particle
/// being attracted with the mass of the one attracting it. Here those are
/// different numbers, and getting them the wrong way round breaks the momentum
/// sum immediately.
[[nodiscard]] ParticleData scattered(RandomSource& random) {
    ParticleData data;
    data.reserve(kParticles);

    for (Index particle = 0; particle < kParticles; ++particle) {
        data.add(3 * random.unit_vector(), Vec3{}, random.uniform(static_cast<Real>(0.5), 2));
    }

    return data;
}

/// `sum_i m_i a_i`, the rate of change of the total linear momentum, together
/// with `sum_i m_i |a_i|`, the scale of the terms it cancelled from.
struct MomentumResidual {
    Vec3 sum;
    Real scale = 0;

    [[nodiscard]] Real relative() const { return norm(sum) / scale; }
};

[[nodiscard]] MomentumResidual momentum_residual(ForceSolver& solver, ParticleData& data) {
    solver.evaluate(data.positions(), data.masses(), data.accelerations());

    const auto accelerations = data.accelerations();
    const auto masses = data.masses();

    MomentumResidual residual;

    // Summed plainly, without the compensation the diagnostics use. That is
    // deliberate: a compensated sum here would hide the very round-off the test
    // is claiming to be the only error present, and would leave the assertion
    // unable to fail for a reason the kernel caused.
    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 term = masses[i] * accelerations.get(i);
        residual.sum += term;
        residual.scale += norm(term);
    }

    return residual;
}

} // namespace

TEST_CASE("the solver conserves linear momentum to round-off", "[property][solvers]") {
    // Newton's third law is not built into this kernel. It computes the pull of j
    // on i and the pull of i on j as two separate sums over two separate ranges,
    // and nothing in the code arranges for them to cancel (ADR-0015). That they do
    // is a statement about the force law being antisymmetric in the separation and
    // about both halves reading the same masses and positions, which is what makes
    // this worth testing rather than a tautology.
    //
    // The residual that remains is round-off in the accumulation, and it is
    // measured against the size of the terms that cancelled rather than against
    // the accelerations themselves.
    for (const std::uint64_t seed : kSeeds) {
        RandomSource random{seed};

        for (const bool sphere : {true, false}) {
            ParticleData data = sphere ? sampled_sphere(random) : scattered(random);
            DirectSolver solver{kSofteningLength};

            const MomentumResidual residual = momentum_residual(solver, data);

            CAPTURE(seed, sphere, kParticles, residual.sum.x, residual.sum.y, residual.sum.z,
                    residual.scale, residual.relative());

            REQUIRE(residual.relative() <= kResidualTolerance);
        }
    }
}

TEST_CASE("a pair pulls on itself equally and oppositely", "[property][solvers]") {
    // The same statement at the smallest size where it says anything, and the one
    // case where it can be read directly rather than through a sum: two particles
    // of different masses feel accelerations in the ratio of those masses, in
    // opposite directions.
    for (const std::uint64_t seed : kSeeds) {
        RandomSource random{seed};

        ParticleData data;
        data.add(2 * random.unit_vector(), Vec3{}, random.uniform(static_cast<Real>(0.5), 2));
        data.add(2 * random.unit_vector(), Vec3{}, random.uniform(static_cast<Real>(0.5), 2));

        DirectSolver solver{kSofteningLength};
        const MomentumResidual residual = momentum_residual(solver, data);

        CAPTURE(seed, residual.sum.x, residual.sum.y, residual.sum.z, residual.scale);

        REQUIRE(residual.relative() <= kResidualTolerance);
    }
}

TEST_CASE("the accelerations do not depend on where the origin is", "[property][solvers]") {
    // Gravity depends on the separations between particles and not on their
    // coordinates, so moving the whole configuration must leave every
    // acceleration unchanged. The property is worth stating because the kernel
    // does not compute separations symmetrically: it subtracts the position of
    // the particle being accelerated from each of the others, and a kernel that
    // had picked up an absolute coordinate somewhere, an origin left in a
    // distance or a sign taken from a coordinate rather than from a difference,
    // would still pass every test that keeps the configuration where it was put.
    //
    // The shift is a few times the size of the configuration rather than a large
    // number. Translating a system far from the origin loses precision in the
    // differences themselves, which is a property of floating-point subtraction
    // rather than of this kernel, and the test would then be measuring that.
    const Vec3 shift{3, -5, 8};

    for (const std::uint64_t seed : kSeeds) {
        RandomSource random{seed};
        ParticleData data = sampled_sphere(random);

        DirectSolver solver{kSofteningLength};
        solver.evaluate(data.positions(), data.masses(), data.accelerations());

        ParticleData moved = data;
        const auto positions = moved.positions();

        for (Index i = 0; i < moved.size(); ++i) {
            positions.set(i, positions.get(i) + shift);
        }

        DirectSolver moved_solver{kSofteningLength};
        moved_solver.evaluate(moved.positions(), moved.masses(), moved.accelerations());

        for (Index i = 0; i < data.size(); ++i) {
            const Vec3 original = data.accelerations().get(i);
            const Vec3 translated = moved.accelerations().get(i);
            const Real error = norm(translated - original);

            CAPTURE(seed, i, error, norm(original));

            REQUIRE(error <= kTranslationTolerance * norm(original));
        }
    }
}
