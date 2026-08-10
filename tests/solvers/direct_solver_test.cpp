#include "orrery/solvers/direct_solver.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/interaction_count.hpp"

namespace {

using orrery::core::Index;
using orrery::core::kGravitationalConstant;
using orrery::core::kSinglePrecision;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::softened_inverse_distance_cubed;
using orrery::core::Softening;
using orrery::core::squared_norm;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::DirectSolver;
using orrery::solvers::InteractionCount;

constexpr std::uint64_t kSeed = 20260810;

constexpr Real kEpsilon = std::numeric_limits<Real>::epsilon();

/// Ask the solver for the accelerations of a configuration, in place.
///
/// The solver writes into whatever buffer it is handed, and the buffer a
/// simulation hands it is the acceleration array of the particle store. Every
/// test here goes through that path rather than through a buffer of its own, so
/// that the spans being exercised are the ones the integrators will pass.
void evaluate(DirectSolver& solver, ParticleData& data) {
    solver.evaluate(data.positions(), data.masses(), data.accelerations());
}

/// Softened pairwise gravity, written again, as plainly as it can be written.
///
/// The comparison against this is only worth making because the two are not the
/// same code. This one gathers each position into a `Vec3` and skips the self
/// term with a branch, where the kernel indexes the component arrays and skips it
/// by summing the two ranges either side. If either has an error in its
/// bookkeeping, the two disagree.
///
/// What they deliberately share is `core/softening.hpp`. The claim being tested
/// is that the kernel sums the right terms, not that the softened potential can
/// be written down twice, and a second copy of that expression is the one thing
/// ADR-0008 exists to prevent.
[[nodiscard]] Vec3 reference_acceleration(const ParticleData& data, Index target,
                                          Softening softening) {
    const auto positions = data.positions();
    const auto masses = data.masses();
    const Index count = data.size();

    Vec3 acceleration{};

    for (Index j = 0; j < count; ++j) {
        if (j == target) {
            continue;
        }

        const Vec3 separation = positions.get(j) - positions.get(target);
        const Real factor = softened_inverse_distance_cubed(squared_norm(separation), softening);
        acceleration += kGravitationalConstant * masses[j] * factor * separation;
    }

    return acceleration;
}

/// A Plummer sphere, which is the configuration the solver will spend its life
/// on and the one whose range of separations is widest.
[[nodiscard]] ParticleData sampled_sphere(Index count, RandomSource& random) {
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

} // namespace

TEST_CASE("the acceleration of a two-body pair is exact", "[validation][solvers]") {
    // The strongest statement this phase makes about correctness, and the reason
    // for these particular numbers. Two masses of 2 and 3 at a separation of 2
    // give an acceleration of 3/4 on one and 1/2 on the other, and every
    // intermediate value along the way, the separation, its square, the square
    // root of that and the reciprocal, is a small exact binary fraction. IEEE 754
    // requires the square root to be correctly rounded, so an exact input gives
    // an exact output, and the whole calculation is exact.
    //
    // The comparison is therefore for equality rather than against a tolerance.
    // There is no tolerance to choose, and a failure here means the formula is
    // wrong rather than that the arithmetic drifted.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{2});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{3});

    DirectSolver solver;
    evaluate(solver, data);

    REQUIRE(data.accelerations().get(0) == Vec3{Real{0.75}, 0, 0});
    REQUIRE(data.accelerations().get(1) == Vec3{Real{-0.5}, 0, 0});
}

TEST_CASE("the acceleration points along the separation in all three components",
          "[validation][solvers]") {
    // The test above is along one axis, so it would pass on a kernel that had
    // dropped or transposed the other two. This pair is offset in all three, at a
    // separation of 13 in units where 3, 4 and 12 form the sides. The expected
    // value is the direction times the magnitude, formed here from the same
    // numbers by a different route.
    ParticleData data;
    data.add(Vec3{}, Vec3{}, Real{1});
    data.add(Vec3{3, 4, 12}, Vec3{}, Real{2});

    DirectSolver solver;
    evaluate(solver, data);

    constexpr Real kDistance = 13;
    const Vec3 expected = Vec3{3, 4, 12} * (2 / (kDistance * kDistance * kDistance));
    const Vec3 measured = data.accelerations().get(0);

    CAPTURE(measured.x, measured.y, measured.z, expected.x, expected.y, expected.z);

    // A few units in the last place. The reciprocal of 13 is not a binary
    // fraction, so unlike the case above this one rounds, and the two expressions
    // round differently.
    REQUIRE(norm(measured - expected) <= 8 * kEpsilon * norm(expected));
}

TEST_CASE("softening reduces the acceleration by the Plummer factor", "[unit][solvers]") {
    // Softening is a change to the physics rather than a guard on the arithmetic,
    // so the test is that the kernel produces the softened law and not merely
    // something smaller than the unsoftened one. The expected value is the
    // closed form written out here in terms of `std::pow`, which is a different
    // route to it than the kernel's reciprocal square root.
    constexpr Real kSeparation = 4;
    constexpr Real kSofteningLength = 3;
    constexpr Real kMass = 5;

    ParticleData data;
    data.add(Vec3{}, Vec3{}, Real{1});
    data.add(Vec3{kSeparation, 0, 0}, Vec3{}, kMass);

    DirectSolver solver{Softening{kSofteningLength}};
    evaluate(solver, data);

    const Real softened_distance_squared =
        (kSeparation * kSeparation) + (kSofteningLength * kSofteningLength);
    const Real expected =
        kMass * kSeparation / std::pow(softened_distance_squared, static_cast<Real>(1.5));

    const Vec3 measured = data.accelerations().get(0);

    CAPTURE(measured.x, expected);

    REQUIRE(std::abs(measured.x - expected) <= 8 * kEpsilon * expected);
    REQUIRE(measured.y == Real{0});
    REQUIRE(measured.z == Real{0});

    // And the physical statement the number encodes: softening weakens the
    // attraction at this separation rather than strengthening it. The unsoftened
    // value is 5/16, the softened one 4/25 of the same mass.
    REQUIRE(measured.x < kMass / (kSeparation * kSeparation));

    REQUIRE(solver.softening().squared() == Softening{kSofteningLength}.squared());
}

TEST_CASE("the solver writes the accelerations rather than accumulating them", "[unit][solvers]") {
    // The interface says the buffer is written, which is what lets a caller pass
    // the store's acceleration array without clearing it first. Every step of
    // every integrator depends on it: the array handed in already holds the
    // accelerations of the previous step, and a kernel that added to what it
    // found would double them and keep going.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{2});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{3});

    const auto accelerations = data.accelerations();
    accelerations.set(0, Vec3{7, 7, 7});
    accelerations.set(1, Vec3{-9, 9, -9});

    DirectSolver solver;
    evaluate(solver, data);

    REQUIRE(accelerations.get(0) == Vec3{Real{0.75}, 0, 0});
    REQUIRE(accelerations.get(1) == Vec3{Real{-0.5}, 0, 0});
}

TEST_CASE("a particle does not attract itself", "[unit][solvers]") {
    // Excluding the self term is not bookkeeping. Unsoftened, a particle's
    // separation from itself is zero and the softened distance is zero with it, so
    // the term is a zero numerator over a zero denominator: a NaN, which would
    // then propagate through the sum into every component of every particle.
    // Softened, it is finite and non-zero and would be a self-acceleration.
    //
    // A single particle is the case where nothing else can mask either failure.
    ParticleData data;
    data.add(Vec3{2, -3, 5}, Vec3{}, Real{4});

    DirectSolver unsoftened;
    evaluate(unsoftened, data);
    REQUIRE(data.accelerations().get(0) == Vec3{});

    DirectSolver softened{Softening{static_cast<Real>(0.1)}};
    evaluate(softened, data);
    REQUIRE(data.accelerations().get(0) == Vec3{});
}

TEST_CASE("an empty configuration is not an error", "[unit][solvers]") {
    // A container that has been created but not yet filled is a configuration
    // with no particles in it, and the solver is asked for its accelerations by
    // the same code that asks for anyone else's. There is nothing to compute and
    // nothing to count, and neither is a reason to fail.
    ParticleData data;
    DirectSolver solver;

    evaluate(solver, data);

    const InteractionCount count = solver.interaction_count();
    REQUIRE(count.evaluations == 1);
    REQUIRE(count.particle_particle == 0);
}

TEST_CASE("the interaction counter reports the work the algorithm did", "[unit][solvers]") {
    // N(N-1) ordered pairs per evaluation, accumulated across evaluations. This
    // is the number every cross-algorithm comparison in the project is expressed
    // in, so it is asserted rather than assumed: a count that drifted from the
    // work actually done would misreport exactly the result it exists to
    // substantiate.
    RandomSource random{kSeed};
    ParticleData data = sampled_sphere(5, random);

    DirectSolver solver;
    REQUIRE(solver.interaction_count().evaluations == 0);
    REQUIRE(solver.interaction_count().particle_particle == 0);

    evaluate(solver, data);
    REQUIRE(solver.interaction_count().evaluations == 1);
    REQUIRE(solver.interaction_count().particle_particle == 20);

    evaluate(solver, data);
    REQUIRE(solver.interaction_count().evaluations == 2);
    REQUIRE(solver.interaction_count().particle_particle == 40);

    solver.reset_interaction_count();
    REQUIRE(solver.interaction_count().evaluations == 0);
    REQUIRE(solver.interaction_count().particle_particle == 0);

    // One particle interacts with nobody, which is the case where the closed-form
    // count would be tempting to write as a product that underflows.
    ParticleData lone;
    lone.add(Vec3{}, Vec3{}, Real{1});
    evaluate(solver, lone);
    REQUIRE(solver.interaction_count().evaluations == 1);
    REQUIRE(solver.interaction_count().particle_particle == 0);

    // Converted rather than compared as a view, for the reason the integrator
    // fixtures give: Catch2 renders a `std::string_view` only when its own
    // library was built against the same standard as the test that uses it.
    REQUIRE(std::string{solver.name()} == "direct");
}

TEST_CASE("the kernel agrees with an independently written summation", "[unit][solvers]") {
    // The analytic tests above pin down two and three particles. This one covers
    // the loop structure at a size where an off-by-one in either range would
    // change the answer without making it obviously wrong, over a configuration
    // with the wide spread of separations a sampled sphere produces.
    RandomSource random{kSeed};
    ParticleData data = sampled_sphere(64, random);

    const Softening softening{static_cast<Real>(0.05)};
    DirectSolver solver{softening};
    evaluate(solver, data);

    // The two implementations sum the same terms in the same order, so they
    // differ only by whatever the compiler does with the arithmetic, such as
    // contracting a multiply and an add into one instruction in one of them. A
    // few units in the last place per particle covers that; anything structural
    // is orders of magnitude larger.
    const Real tolerance = kSinglePrecision ? static_cast<Real>(1e-5) : static_cast<Real>(1e-13);

    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 expected = reference_acceleration(data, i, softening);
        const Vec3 measured = data.accelerations().get(i);
        const Real error = norm(measured - expected);

        CAPTURE(kSeed, i, error, norm(expected), measured.x, measured.y, measured.z, expected.x,
                expected.y, expected.z);

        REQUIRE(error <= tolerance * norm(expected));
    }
}
