#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/barnes_hut_solver.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/direct_solver.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"
#include "orrery/solvers/octree.hpp"
#include "orrery/solvers/reference_kernel.hpp"

namespace {

using orrery::backend::ThreadPool;
using orrery::backend::WorkStealingExecutor;
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
using orrery::solvers::BarnesHutSolver;
using orrery::solvers::DirectSolver;
using orrery::solvers::ForceSolver;
using orrery::solvers::InteractionCount;
using orrery::solvers::KernelKind;
using orrery::solvers::reference_acceleration;
using orrery::solvers::ReferenceAcceleration;
using orrery::solvers::TreeParameters;

constexpr std::uint64_t kSeed = 20260810;

/// The softening a run of a sampled sphere would use.
///
/// Non-zero throughout, because the comparison being made everywhere below is
/// between two ways of summing the same force law and a close pair would
/// otherwise make the comparison a measurement of how the two rounded a
/// near-singular term.
const Softening kSoftening{static_cast<Real>(0.05)};

[[nodiscard]] ParticleData sampled_sphere(Index count, RandomSource& random) {
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

void evaluate(ForceSolver& solver, ParticleData& data) {
    solver.evaluate(data.positions(), data.masses(), data.accelerations());
}

/// How wrong the tree solver's answer is, against the compensated reference.
///
/// Reported relative to the size of the acceleration of the particle
/// concerned, and summarised as the worst case and the root mean square. The
/// worst case is what an error bound has to cover; the mean square is what
/// actually moves an orbit, and the two differ by an order of magnitude for a
/// tree because the particles in the dense core are approximated much better
/// than the ones in the halo.
struct ErrorSummary {
    double worst{};
    double root_mean_square{};
};

[[nodiscard]] ErrorSummary measure_error(const ParticleData& data, Softening softening) {
    ErrorSummary summary;
    double total = 0;

    for (Index i = 0; i < data.size(); ++i) {
        const ReferenceAcceleration exact =
            reference_acceleration(data.positions(), data.masses(), i, softening);
        const Vec3 measured = data.accelerations().get(i);

        const double dx = static_cast<double>(measured.x) - exact.x;
        const double dy = static_cast<double>(measured.y) - exact.y;
        const double dz = static_cast<double>(measured.z) - exact.z;

        const double magnitude =
            std::sqrt((exact.x * exact.x) + (exact.y * exact.y) + (exact.z * exact.z));
        const double error = std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / magnitude;

        summary.worst = std::max(summary.worst, error);
        total += error * error;
    }

    summary.root_mean_square = std::sqrt(total / static_cast<double>(data.size()));

    return summary;
}

} // namespace

TEST_CASE("an opening angle of zero reproduces direct summation", "[validation][solvers]") {
    // The strongest correctness statement this phase makes, and the reason the
    // opening angle is allowed to be zero at all. With no cell ever accepted,
    // the walk descends to every leaf and sums every pair, so the tree solver
    // computes exactly what the direct solver computes and by a route that
    // shares none of its code: a different order of particles, a different loop
    // and a different set of ranges.
    //
    // The two do not agree bit for bit and cannot. The tree sums the pairs of
    // one target in the order the leaves appear in the tree, and direct
    // summation sums them in index order, so the two reassociate the same terms
    // differently. What is asserted is that the difference is of the size
    // reassociation produces and not of the size a missing or duplicated
    // interaction would.
    RandomSource random{kSeed};
    ParticleData tree_data = sampled_sphere(600, random);
    ParticleData direct_data = tree_data;

    BarnesHutSolver tree{TreeParameters{.opening_angle = 0, .leaf_capacity = 8}, kSoftening};
    DirectSolver direct{kSoftening};

    evaluate(tree, tree_data);
    evaluate(direct, direct_data);

    const Real tolerance = kSinglePrecision ? static_cast<Real>(2e-5) : static_cast<Real>(2e-13);

    for (Index i = 0; i < tree_data.size(); ++i) {
        const Vec3 approximate = tree_data.accelerations().get(i);
        const Vec3 exact = direct_data.accelerations().get(i);
        const Real error = norm(approximate - exact);

        CAPTURE(kSeed, i, error, norm(exact));

        REQUIRE(error <= tolerance * norm(exact));
    }

    // And it did so by summing every pair, which is the other half of the
    // claim: the same answer reached by accepting no cells at all.
    const InteractionCount count = tree.interaction_count();
    REQUIRE(count.particle_cell == 0);
    REQUIRE(count.particle_particle == std::uint64_t{600} * 599);
}

TEST_CASE("the error falls as the opening angle closes", "[validation][solvers]") {
    // The curve the whole method is judged on. Each angle is measured against
    // the compensated reference rather than against the next angle, so what is
    // reported is the approximation's own error and not the difference between
    // two approximations.
    RandomSource random{kSeed};
    const ParticleData sphere = sampled_sphere(1000, random);

    const std::array<Real, 4> angles{static_cast<Real>(0.8), static_cast<Real>(0.5),
                                     static_cast<Real>(0.3), static_cast<Real>(0.1)};

    std::vector<ErrorSummary> errors;

    for (const Real angle : angles) {
        ParticleData data = sphere;
        BarnesHutSolver solver{TreeParameters{.opening_angle = angle}, kSoftening};
        evaluate(solver, data);

        const ErrorSummary summary = measure_error(data, kSoftening);
        CAPTURE(kSeed, angle, summary.worst, summary.root_mean_square);

        // Even the loosest angle has to be usable. Ten per cent on the worst
        // particle is far inside what a correct implementation delivers at 0.8,
        // where the measured figure is under four, and far outside what any
        // bookkeeping error survives.
        REQUIRE(summary.worst < 1e-1);

        errors.push_back(summary);
    }

    // Monotone in both statistics. This is the property that says the opening
    // angle controls the error rather than merely correlating with it, and it
    // is the one a criterion that measured distance from the wrong point would
    // break first.
    for (std::size_t i = 1; i < errors.size(); ++i) {
        CAPTURE(i, errors[i - 1].worst, errors[i].worst, errors[i - 1].root_mean_square,
                errors[i].root_mean_square);

        REQUIRE(errors[i].worst < errors[i - 1].worst);
        REQUIRE(errors[i].root_mean_square < errors[i - 1].root_mean_square);
    }

    // The default this project ships, stated as a number rather than left to
    // be inferred from the shape of the curve.
    REQUIRE(errors[1].root_mean_square < 1e-2);
}

TEST_CASE("the quadrupole moment improves the accuracy it costs for", "[validation][solvers]") {
    // The accuracy option has to earn its place: the next term of the expansion
    // must reduce the error rather than merely change it, and a sign error in
    // the contraction would produce a term of the right size and the wrong
    // direction, which would not.
    RandomSource random{kSeed};
    const ParticleData sphere = sampled_sphere(1000, random);

    const Real angle = static_cast<Real>(0.6);

    ParticleData monopole_data = sphere;
    BarnesHutSolver monopole{TreeParameters{.opening_angle = angle}, kSoftening};
    evaluate(monopole, monopole_data);

    ParticleData quadrupole_data = sphere;
    BarnesHutSolver quadrupole{TreeParameters{.opening_angle = angle, .quadrupole = true},
                               kSoftening};
    evaluate(quadrupole, quadrupole_data);

    const ErrorSummary without = measure_error(monopole_data, kSoftening);
    const ErrorSummary with = measure_error(quadrupole_data, kSoftening);

    CAPTURE(without.worst, with.worst, without.root_mean_square, with.root_mean_square);

    // A factor of two is well inside what the term delivers at this angle and
    // is not reachable by accident.
    REQUIRE(with.root_mean_square * 2 < without.root_mean_square);
    REQUIRE(with.worst < without.worst);

    // The two solvers accepted the same cells, which is what makes the
    // comparison a comparison of the expansion rather than of two different
    // traversals.
    REQUIRE(monopole.interaction_count().particle_cell ==
            quadrupole.interaction_count().particle_cell);
}

TEST_CASE("a two-body pair is exact", "[validation][solvers]") {
    // Two particles are one leaf, so the tree solver computes the pair
    // directly and must reproduce the analytic result to the last bit, exactly
    // as the direct solver does. A tree that approximated here would be
    // approximating a configuration with nothing to approximate.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{2});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{3});

    BarnesHutSolver solver;
    evaluate(solver, data);

    REQUIRE(data.accelerations().get(0) == Vec3{Real{0.75}, 0, 0});
    REQUIRE(data.accelerations().get(1) == Vec3{Real{-0.5}, 0, 0});

    REQUIRE(solver.interaction_count().particle_particle == 2);
    REQUIRE(solver.interaction_count().particle_cell == 0);
}

TEST_CASE("a particle does not attract itself through its own cell", "[unit][solvers]") {
    // The hazard the opening angle's upper bound exists to prevent. Above one,
    // a cell can be accepted while the particle being accelerated is inside it,
    // and the particle is then accelerated towards a centre of mass that
    // includes its own. Unsoftened it is worse than wrong: the separation can
    // be zero and the answer a NaN that propagates through every later step.
    ParticleData data;
    data.add(Vec3{2, -3, 5}, Vec3{}, Real{4});

    BarnesHutSolver solver{TreeParameters{.opening_angle = 10}};
    evaluate(solver, data);

    REQUIRE(solver.parameters().opening_angle == Real{1});
    REQUIRE(data.accelerations().get(0) == Vec3{});

    // And a crowd of particles at one point, which is the configuration where
    // every cell is degenerate at once.
    ParticleData crowd;
    for (int i = 0; i < 20; ++i) {
        crowd.add(Vec3{1, 1, 1}, Vec3{}, Real{1});
    }

    BarnesHutSolver crowded{TreeParameters{.opening_angle = 1}, Softening{Real{1}}};
    evaluate(crowded, crowd);

    for (Index i = 0; i < crowd.size(); ++i) {
        CAPTURE(i);
        REQUIRE(crowd.accelerations().get(i) == Vec3{});
    }
}

TEST_CASE("an empty configuration has no tree and no interactions", "[unit][solvers]") {
    ParticleData data;
    BarnesHutSolver solver;

    evaluate(solver, data);

    const InteractionCount count = solver.interaction_count();
    REQUIRE(count.evaluations == 1);
    REQUIRE(count.particle_particle == 0);
    REQUIRE(count.particle_cell == 0);
    REQUIRE(solver.tree().empty());

    REQUIRE(std::string{solver.name()} == "barnes-hut");
}

TEST_CASE("the tree solver writes the accelerations rather than accumulating them",
          "[unit][solvers]") {
    // As for the direct solver, and for the same reason: every integrator step
    // hands over an array holding the previous step's accelerations. The tree
    // solver writes through a permutation, which is the additional way this
    // could go wrong, so a particle left untouched would be a particle whose
    // index the permutation never produced.
    RandomSource random{kSeed};
    ParticleData data = sampled_sphere(300, random);

    for (Index i = 0; i < data.size(); ++i) {
        data.accelerations().set(i, Vec3{7, -7, 7});
    }

    BarnesHutSolver solver{TreeParameters{}, kSoftening};
    evaluate(solver, data);

    for (Index i = 0; i < data.size(); ++i) {
        CAPTURE(i);
        REQUIRE(data.accelerations().get(i) != Vec3{7, -7, 7});
    }
}

TEST_CASE("the answer does not depend on the threads that computed it", "[property][solvers]") {
    // Bit for bit, as for the direct solver. The tree solver has three more
    // ways to fail this than the direct solver does: the sort, the
    // construction and the permutation the accelerations are written back
    // through. Each is deterministic on its own and this is the assertion that
    // the composition is.
    RandomSource random{kSeed};
    const ParticleData sphere = sampled_sphere(20000, random);

    ParticleData serial_data = sphere;
    BarnesHutSolver serial{TreeParameters{.quadrupole = true}, kSoftening};
    evaluate(serial, serial_data);

    WorkStealingExecutor executor{ThreadPool::default_worker_count()};
    ParticleData parallel_data = sphere;
    BarnesHutSolver parallel{TreeParameters{.quadrupole = true}, kSoftening, executor};
    evaluate(parallel, parallel_data);

    for (Index i = 0; i < sphere.size(); ++i) {
        CAPTURE(kSeed, i);
        REQUIRE(serial_data.accelerations().get(i) == parallel_data.accelerations().get(i));
    }

    REQUIRE(serial.interaction_count().particle_particle ==
            parallel.interaction_count().particle_particle);
    REQUIRE(serial.interaction_count().particle_cell == parallel.interaction_count().particle_cell);
}

TEST_CASE("the kernel at the leaves does not change the physics", "[property][solvers]") {
    // The leaves are summed by the direct kernel of Phase 7, which has a scalar
    // form and a vector form that differ in the last bits by construction. Both
    // have to produce the same accelerations to the accuracy that difference
    // implies, or the tree's leaf handling depends on the instruction set.
    RandomSource random{kSeed};
    const ParticleData sphere = sampled_sphere(2000, random);

    ParticleData scalar_data = sphere;
    BarnesHutSolver scalar{TreeParameters{}, kSoftening};
    scalar.select_kernel(KernelKind::kScalar);
    evaluate(scalar, scalar_data);

    ParticleData vector_data = sphere;
    BarnesHutSolver vector{TreeParameters{}, kSoftening};
    vector.select_kernel(KernelKind::kAvx2);
    evaluate(vector, vector_data);

    REQUIRE(scalar.kernel() == KernelKind::kScalar);

    const Real tolerance = kSinglePrecision ? static_cast<Real>(1e-5) : static_cast<Real>(1e-13);

    for (Index i = 0; i < sphere.size(); ++i) {
        const Vec3 one = scalar_data.accelerations().get(i);
        const Vec3 other = vector_data.accelerations().get(i);

        CAPTURE(kSeed, i, norm(one - other), norm(one));

        REQUIRE(norm(one - other) <= tolerance * norm(one));
    }

    // The counts are a property of the tree rather than of the kernel, so they
    // are identical whichever kernel summed the pairs.
    REQUIRE(scalar.interaction_count().particle_particle ==
            vector.interaction_count().particle_particle);
}

TEST_CASE("the tree computes far fewer interactions than direct summation",
          "[validation][solvers]") {
    // The reason for the whole phase, stated as the count that does not depend
    // on the machine it was measured on. The ratio grows with N, so four
    // thousand particles is a lower bound on what the method buys rather than a
    // headline.
    RandomSource random{kSeed};
    const ParticleData sphere = sampled_sphere(4000, random);

    const auto counts = [&](Index leaf_capacity) {
        ParticleData data = sphere;
        BarnesHutSolver solver{TreeParameters{.leaf_capacity = leaf_capacity}, kSoftening};
        evaluate(solver, data);

        return solver.interaction_count();
    };

    const InteractionCount wide = counts(32);
    const InteractionCount narrow = counts(4);

    const std::uint64_t total = wide.particle_particle + wide.particle_cell;
    const std::uint64_t direct = std::uint64_t{4000} * 3999;

    CAPTURE(wide.particle_particle, wide.particle_cell, total, direct);

    REQUIRE(wide.particle_cell > 0);
    REQUIRE(total * 2 < direct);

    // Where the work sits, and the finding this assertion exists to record.
    // With the leaf capacity this project defaults to, most of the interactions
    // are particle-particle rather than particle-cell: a leaf that has to be
    // opened contributes all of its particles, and a leaf holding thirty-two of
    // them contributes thirty-two.
    //
    // That is the trade the capacity is chosen for rather than a failure of the
    // opening angle. A particle-particle interaction is one iteration of the
    // vectorised kernel over contiguous memory and a particle-cell interaction
    // is a scalar term at the end of a walk, so moving work from the second to
    // the first is worth doing even though it raises the total.
    // `docs/performance/barnes_hut.md` measures what it is worth in time.
    REQUIRE(wide.particle_particle > wide.particle_cell);

    // And the balance moves the other way when the leaves are made small, which
    // is what says the effect above is the capacity rather than the criterion.
    REQUIRE(narrow.particle_particle < wide.particle_particle);
    REQUIRE(narrow.particle_cell > wide.particle_cell);
}

TEST_CASE("momentum is conserved only as well as the approximation allows", "[property][solvers]") {
    // Direct summation conserves total momentum to round-off because it
    // computes each pair from both ends. A tree does not: particle i may see j
    // through a cell while j sees i directly, so the two forces are not equal
    // and opposite. Asserting exact conservation here would be asserting
    // something false, and asserting nothing would leave the project's most
    // quotable invariant unmeasured for its main solver.
    //
    // So what is asserted is the size of the violation, relative to the scale
    // of the forces being summed, and that closing the opening angle reduces
    // it.
    RandomSource random{kSeed};
    const ParticleData sphere = sampled_sphere(2000, random);

    const auto momentum_error = [&](Real angle) {
        ParticleData data = sphere;
        BarnesHutSolver solver{TreeParameters{.opening_angle = angle}, kSoftening};
        evaluate(solver, data);

        Vec3 total{};
        Real scale = 0;

        for (Index i = 0; i < data.size(); ++i) {
            const Vec3 force = data.accelerations().get(i) * data.masses()[i];
            total += force;
            scale += norm(force);
        }

        return norm(total) / scale;
    };

    const Real loose = momentum_error(static_cast<Real>(0.8));
    const Real tight = momentum_error(static_cast<Real>(0.2));

    CAPTURE(loose, tight);

    // Small in absolute terms at the default sort of angle, and smaller still
    // when the angle closes. The second is the statement that the violation is
    // the approximation rather than a bug in the summation.
    REQUIRE(loose < static_cast<Real>(1e-3));
    REQUIRE(tight < loose);

    // And direct summation on the same configuration, for the contrast the
    // comparison is being drawn against.
    ParticleData direct_data = sphere;
    DirectSolver direct{kSoftening};
    evaluate(direct, direct_data);

    Vec3 total{};
    Real scale = 0;

    for (Index i = 0; i < direct_data.size(); ++i) {
        const Vec3 force = direct_data.accelerations().get(i) * direct_data.masses()[i];
        total += force;
        scale += norm(force);
    }

    const Real round_off = 64 * std::numeric_limits<Real>::epsilon();
    CAPTURE(norm(total) / scale, round_off);

    REQUIRE(norm(total) / scale < round_off);
}

TEST_CASE("softening reaches the cells as well as the pairs", "[unit][solvers]") {
    // A tree solver has two places to apply the softened force law and would
    // pass every test above with it applied to only one of them, since the
    // difference shows up only at separations comparable with the softening
    // length. Here the softening is large enough to matter at every separation
    // in the configuration, so an unsoftened cell term would stand out.
    RandomSource random{kSeed};
    ParticleData tree_data = sampled_sphere(500, random);
    ParticleData direct_data = tree_data;

    const Softening coarse{static_cast<Real>(0.5)};

    BarnesHutSolver tree{TreeParameters{.opening_angle = static_cast<Real>(0.3)}, coarse};
    DirectSolver direct{coarse};

    evaluate(tree, tree_data);
    evaluate(direct, direct_data);

    REQUIRE(tree.softening().squared() == coarse.squared());
    REQUIRE(tree.interaction_count().particle_cell > 0);

    for (Index i = 0; i < tree_data.size(); ++i) {
        const Vec3 approximate = tree_data.accelerations().get(i);
        const Vec3 exact = direct_data.accelerations().get(i);

        CAPTURE(i, norm(approximate - exact), norm(exact));

        REQUIRE(norm(approximate - exact) < static_cast<Real>(1e-2) * norm(exact));
    }
}

TEST_CASE("the timings account for the evaluation", "[unit][solvers]") {
    // The breakdown exists so that a tree that is slower than expected can be
    // diagnosed without a profiler. What is asserted is that every phase was
    // measured and that the traversal is the one that dominates, which is the
    // statement that the construction is not where the time goes.
    RandomSource random{kSeed};
    ParticleData data = sampled_sphere(20000, random);

    BarnesHutSolver solver{TreeParameters{}, kSoftening};
    evaluate(solver, data);

    const auto& timings = solver.timings();

    CAPTURE(timings.ordering.count(), timings.gathering.count(), timings.construction.count(),
            timings.traversal.count());

    REQUIRE(timings.ordering.count() > 0);
    REQUIRE(timings.construction.count() > 0);
    REQUIRE(timings.traversal.count() > 0);

    REQUIRE(timings.traversal > timings.ordering + timings.construction + timings.gathering);
}

TEST_CASE("the tree is rebuilt for each evaluation", "[unit][solvers]") {
    // A solver is asked for accelerations again and again on particles that
    // have moved, and a tree built for where they used to be would give a wrong
    // answer that got worse rather than an error that could be seen. The test
    // moves the configuration between evaluations and asks for the same answer
    // the same configuration gives from a fresh solver.
    RandomSource random{kSeed};
    ParticleData data = sampled_sphere(1000, random);

    BarnesHutSolver reused{TreeParameters{}, kSoftening};
    evaluate(reused, data);

    for (Index i = 0; i < data.size(); ++i) {
        data.positions().set(i, data.positions().get(i) * Real{2} + Vec3{5, -3, 1});
    }

    evaluate(reused, data);

    ParticleData fresh_data = data;
    BarnesHutSolver fresh{TreeParameters{}, kSoftening};
    evaluate(fresh, fresh_data);

    for (Index i = 0; i < data.size(); ++i) {
        CAPTURE(i);
        REQUIRE(data.accelerations().get(i) == fresh_data.accelerations().get(i));
    }

    REQUIRE(reused.interaction_count().evaluations == 2);
}
