#include "orrery/solvers/tree_walk.hpp"

#include <cmath>
#include <limits>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/core/particle_data.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/morton.hpp"
#include "orrery/solvers/octree.hpp"

namespace {

using orrery::core::Index;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::Real;
using orrery::core::softened_inverse_distance_cubed;
using orrery::core::Softening;
using orrery::core::squared_norm;
using orrery::core::Vec3;
using orrery::solvers::accumulate_range_scalar;
using orrery::solvers::monopole_acceleration;
using orrery::solvers::MortonOrdering;
using orrery::solvers::Octree;
using orrery::solvers::Quadrupole;
using orrery::solvers::quadrupole_acceleration;
using orrery::solvers::TreeParameters;
using orrery::solvers::walk_tree;
using orrery::solvers::WalkCounts;

/// The exact acceleration at `target` from every particle, one pair at a time.
///
/// Written here rather than reused from the direct solver because what is being
/// checked in this file is the multipole expansion against the sum it stands in
/// for, and the two have to be independent for the comparison to say anything.
[[nodiscard]] Vec3 exact_acceleration(const ParticleData& data, Vec3 target, Softening softening) {
    Vec3 acceleration{};

    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 separation = data.positions().get(i) - target;
        acceleration += separation * (data.masses()[i] * softened_inverse_distance_cubed(
                                                             squared_norm(separation), softening));
    }

    return acceleration;
}

} // namespace

TEST_CASE("the monopole term is one pairwise interaction", "[unit][solvers]") {
    // A cell holding one particle must produce exactly what the direct kernel
    // produces for that particle. Not nearly: the same expression on the same
    // numbers, so the comparison is for equality and a failure means the two
    // are computing different force laws rather than rounding differently.
    ParticleData data;
    data.add(Vec3{3, 4, 12}, Vec3{}, Real{2});

    const Vec3 target{-1, 2, 1};
    const Softening softening{static_cast<Real>(0.3)};

    const Vec3 cell =
        monopole_acceleration(data.positions().get(0) - target, data.masses()[0], softening);
    const Vec3 pair =
        accumulate_range_scalar(data.positions(), data.masses(), target, 0, 1, softening);

    REQUIRE(cell == pair);
}

TEST_CASE("the quadrupole term is the next order of the same expansion", "[validation][solvers]") {
    // Two equal masses either side of the origin, seen from a point on the axis
    // they lie along. The exact acceleration expands as
    //
    //     -2m/R^2 - 6 m a^2 / R^4 - O(a^4 / R^6)
    //
    // so the monopole term alone is wrong at order (a/R)^2 and the quadrupole
    // correction is exactly the term that cancels it. This is the test that
    // pins down the factor of 5/2, the sign of the contraction and the powers
    // of the distance all at once: get any one of them wrong and the correction
    // has the wrong size or the wrong direction, and the error grows rather
    // than falling.
    constexpr Real kOffset = 1;
    constexpr Real kMass = 3;

    ParticleData data;
    data.add(Vec3{kOffset, 0, 0}, Vec3{}, kMass);
    data.add(Vec3{-kOffset, 0, 0}, Vec3{}, kMass);

    // The moment of that pair about the origin, written out by hand from the
    // definition rather than taken from the tree, so that this test checks the
    // expansion and `octree_test.cpp` checks the accumulation.
    const Quadrupole moment{.xx = 4 * kMass * kOffset * kOffset,
                            .xy = 0,
                            .xz = 0,
                            .yy = -2 * kMass * kOffset * kOffset,
                            .yz = 0,
                            .zz = -2 * kMass * kOffset * kOffset};

    const Softening softening;

    for (const Real distance : {Real{8}, Real{16}, Real{32}}) {
        const Vec3 target{distance, 0, 0};
        const Vec3 offset = Vec3{} - target;

        const Vec3 exact = exact_acceleration(data, target, softening);
        const Vec3 monopole = monopole_acceleration(offset, 2 * kMass, softening);
        const Vec3 corrected = monopole + quadrupole_acceleration(offset, moment, softening);

        const Real monopole_error = norm(monopole - exact) / norm(exact);
        const Real corrected_error = norm(corrected - exact) / norm(exact);

        CAPTURE(distance, monopole_error, corrected_error);

        // The correction has to reduce the error by the ratio the orders
        // predict. At a separation of eight offsets the monopole error is of
        // order (1/8)^2 and what is left after the correction is of order
        // (1/8)^4, which is a factor of sixty-four.
        REQUIRE(corrected_error * 32 < monopole_error);
    }

    // And off the axis, where the tensor is not diagonal in the direction of
    // the offset and every stored component is exercised.
    const Vec3 target{6, 5, -4};
    const Vec3 offset = Vec3{} - target;

    const Vec3 exact = exact_acceleration(data, target, softening);
    const Vec3 monopole = monopole_acceleration(offset, 2 * kMass, softening);
    const Vec3 corrected = monopole + quadrupole_acceleration(offset, moment, softening);

    CAPTURE(norm(monopole - exact) / norm(exact), norm(corrected - exact) / norm(exact));

    REQUIRE(norm(corrected - exact) * 8 < norm(monopole - exact));
}

TEST_CASE("a symmetric cell has no correction to make", "[unit][solvers]") {
    // The quadrupole of a spherically symmetric distribution is zero, so the
    // correction is exactly zero rather than small. A term that added something
    // to a zero tensor would be adding the softening or the distance rather
    // than the moment.
    const Quadrupole moment;

    const Vec3 correction =
        quadrupole_acceleration(Vec3{3, -4, 5}, moment, Softening{static_cast<Real>(0.1)});

    REQUIRE(correction == Vec3{});
}

TEST_CASE("a walk of an empty tree finds nothing", "[unit][solvers]") {
    const ParticleData data;

    MortonOrdering ordering;
    ordering.build(data.positions(), nullptr);

    Octree tree;
    tree.build(data.positions(), data.masses(), ordering.keys(), ordering.cube(), TreeParameters{},
               nullptr);

    WalkCounts counts;
    const Vec3 acceleration = walk_tree(tree, data.positions(), data.masses(), 0, Softening{},
                                        accumulate_range_scalar, counts);

    REQUIRE(acceleration == Vec3{});
    REQUIRE(counts.particle_particle == 0);
    REQUIRE(counts.particle_cell == 0);
}

TEST_CASE("the walk counts what it computed", "[unit][solvers]") {
    // Eight particles in one leaf, walked by one of them. Every other particle
    // is summed directly and the walker is skipped, so the count is seven and
    // no cell is accepted, whatever the opening angle says: a cell containing
    // the target can never be far enough away.
    ParticleData data;

    for (int i = 0; i < 8; ++i) {
        data.add(Vec3{static_cast<Real>(i), 0, 0}, Vec3{}, Real{1});
    }

    MortonOrdering ordering;
    ordering.build(data.positions(), nullptr);

    ParticleData sorted{data.size()};
    for (Index i = 0; i < data.size(); ++i) {
        sorted.positions().set(i, data.positions().get(ordering.keys()[i].index));
        sorted.masses()[i] = data.masses()[ordering.keys()[i].index];
    }

    Octree tree;
    tree.build(sorted.positions(), sorted.masses(), ordering.keys(), ordering.cube(),
               TreeParameters{.opening_angle = 1, .leaf_capacity = 8}, nullptr);

    REQUIRE(tree.nodes().size() == 1);

    WalkCounts counts;
    const Vec3 acceleration = walk_tree(tree, sorted.positions(), sorted.masses(), 3, Softening{},
                                        accumulate_range_scalar, counts);

    REQUIRE(counts.particle_particle == 7);
    REQUIRE(counts.particle_cell == 0);

    // The sum over the other seven, written out here with the self term left
    // out by a branch rather than by a range, which is the independent way of
    // arranging the same exclusion.
    Vec3 expected{};

    for (Index i = 0; i < sorted.size(); ++i) {
        if (i == 3) {
            continue;
        }

        const Vec3 separation = sorted.positions().get(i) - sorted.positions().get(3);
        expected += separation * (sorted.masses()[i] * softened_inverse_distance_cubed(
                                                           squared_norm(separation), Softening{}));
    }

    CAPTURE(acceleration.x, expected.x);

    REQUIRE(std::abs(acceleration.x - expected.x) <=
            8 * std::abs(expected.x) * std::numeric_limits<Real>::epsilon());
    REQUIRE(acceleration.y == Real{0});
    REQUIRE(acceleration.z == Real{0});
}
