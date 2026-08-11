#include "orrery/solvers/morton.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"

namespace {

using orrery::backend::ThreadPool;
using orrery::backend::WorkStealingExecutor;
using orrery::core::Index;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::bounding_cube;
using orrery::solvers::BoundingCube;
using orrery::solvers::kMortonBitsPerAxis;
using orrery::solvers::kMortonGridSize;
using orrery::solvers::morton_code;
using orrery::solvers::morton_octant;
using orrery::solvers::MortonCode;
using orrery::solvers::MortonKey;
using orrery::solvers::MortonOrdering;
using orrery::solvers::spread_bits;

constexpr std::uint64_t kSeed = 20260810;

} // namespace

TEST_CASE("bits are spread to every third position", "[unit][solvers]") {
    // The property the dilation has to have, stated on the smallest inputs that
    // can distinguish it from a shift or a multiply. One bit at position n ends
    // up at position 3n, and nothing else moves.
    REQUIRE(spread_bits(0) == 0);
    REQUIRE(spread_bits(1) == 1);
    REQUIRE(spread_bits(2) == 0b1000);
    REQUIRE(spread_bits(3) == 0b1001);
    REQUIRE(spread_bits(0b101) == 0b1000001);

    // The top bit of the 21 the grid uses, which is where an off-by-one in the
    // masks would show up and nowhere else.
    const MortonCode top = spread_bits(1U << 20U);
    REQUIRE(top == MortonCode{1} << 60U);

    // Every one of the 21 bits, separately, so that a mask that dropped one in
    // the middle cannot pass.
    for (unsigned bit = 0; bit < kMortonBitsPerAxis; ++bit) {
        CAPTURE(bit);
        REQUIRE(spread_bits(1U << bit) == MortonCode{1} << (3 * bit));
    }

    // Bits above the grid are ignored rather than allowed to collide with the
    // low ones of the next axis.
    REQUIRE(spread_bits(1U << 21U) == 0);
}

TEST_CASE("the code interleaves the three axes in a stated order", "[unit][solvers]") {
    // x is the most significant of each triple, then y, then z. The order is
    // arbitrary but `octree.cpp` derives the geometry of a child from it, so it
    // is pinned down here rather than left to the implementation.
    REQUIRE(morton_code(0, 0, 0) == 0);
    REQUIRE(morton_code(1, 0, 0) == 0b100);
    REQUIRE(morton_code(0, 1, 0) == 0b010);
    REQUIRE(morton_code(0, 0, 1) == 0b001);
    REQUIRE(morton_code(1, 1, 1) == 0b111);

    // Two levels, so that the ordering between levels is fixed as well: the
    // coarse bit of x outranks the fine bits of everything.
    REQUIRE(morton_code(2, 0, 0) == 0b100'000);
    REQUIRE(morton_code(2, 1, 1) == 0b100'011);
}

TEST_CASE("the octant of a code is read from the level's triple", "[unit][solvers]") {
    // Level 1 is the coarsest and the top triple of the 63 bits in use, which
    // is what makes sorting by code a sort by the coarsest subdivision first.
    const MortonCode corner = morton_code(kMortonGridSize - 1, 0, kMortonGridSize - 1);

    for (unsigned level = 1; level <= kMortonBitsPerAxis; ++level) {
        CAPTURE(level);
        REQUIRE(morton_octant(corner, level) == 0b101);
    }

    // A code with one bit set at one level, to show the levels are independent.
    const MortonCode single = morton_code(1U << 18U, 0, 0);
    REQUIRE(morton_octant(single, 1) == 0);
    REQUIRE(morton_octant(single, 2) == 0);
    REQUIRE(morton_octant(single, 3) == 0b100);
    REQUIRE(morton_octant(single, 4) == 0);

    // Level zero is the root, which is not an octant of anything.
    REQUIRE(morton_octant(corner, 0) == 0);
    REQUIRE(morton_octant(corner, kMortonBitsPerAxis + 1) == 0);
}

TEST_CASE("the bounding cube contains the configuration", "[unit][solvers]") {
    ParticleData data;
    data.add(Vec3{-2, 0, 1}, Vec3{}, Real{1});
    data.add(Vec3{4, 1, 3}, Vec3{}, Real{1});

    const BoundingCube cube = bounding_cube(data.positions());

    // The longest extent is 6, along x, so the cube is 6 on every side and
    // centred on the box rather than anchored at its low corner.
    REQUIRE(cube.size == Real{6});
    REQUIRE(cube.centre().x == Real{1});
    REQUIRE(cube.centre().y == Real{0.5});
    REQUIRE(cube.centre().z == Real{2});

    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 position = data.positions().get(i);
        CAPTURE(i, position.x, position.y, position.z);

        REQUIRE(position.x >= cube.origin.x);
        REQUIRE(position.y >= cube.origin.y);
        REQUIRE(position.z >= cube.origin.z);
        REQUIRE(position.x <= cube.origin.x + cube.size);
        REQUIRE(position.y <= cube.origin.y + cube.size);
        REQUIRE(position.z <= cube.origin.z + cube.size);
    }
}

TEST_CASE("a configuration with no extent still gets a cube", "[unit][solvers]") {
    // Every particle at one point. A cube of zero size would divide by zero in
    // the grid mapping and, worse, would give every cell an acceptance radius
    // of zero, which would let the tree accept a cell containing the particle
    // being accelerated. A positive size costs nothing and removes the case.
    ParticleData data;
    data.add(Vec3{3, 3, 3}, Vec3{}, Real{1});
    data.add(Vec3{3, 3, 3}, Vec3{}, Real{1});

    const BoundingCube cube = bounding_cube(data.positions());

    REQUIRE(cube.size > Real{0});
    REQUIRE(cube.centre().x == Real{3});
    REQUIRE(cube.centre().y == Real{3});
    REQUIRE(cube.centre().z == Real{3});

    // Indistinguishable points get indistinguishable codes, which is the honest
    // answer and the one the tree builder is written to survive.
    REQUIRE(morton_code(Vec3{3, 3, 3}, cube) == morton_code(Vec3{3, 3, 3}, cube));
}

TEST_CASE("an empty configuration has an empty cube", "[unit][solvers]") {
    const ParticleData data;
    const BoundingCube cube = bounding_cube(data.positions());

    REQUIRE(cube.size == Real{0});

    // And a code can still be asked for, because the ordering computes codes
    // before it knows whether there are any.
    REQUIRE(morton_code(Vec3{}, cube) == 0);
}

TEST_CASE("positions on the far face stay inside the grid", "[unit][solvers]") {
    // The particle that defined the bounding box sits exactly on the cube's
    // face, so the mapping sends it to one past the last cell before the clamp.
    // Without the clamp its code would carry a bit outside the 21 the axis has
    // room for and would collide with the next axis.
    const BoundingCube cube{.origin = Vec3{}, .size = 1};

    const MortonCode corner = morton_code(Vec3{1, 1, 1}, cube);
    const MortonCode last =
        morton_code(kMortonGridSize - 1, kMortonGridSize - 1, kMortonGridSize - 1);

    REQUIRE(corner == last);
    REQUIRE(morton_code(Vec3{-1, -1, -1}, cube) == 0);
}

TEST_CASE("the ordering is a permutation with ascending codes", "[unit][solvers]") {
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 500}, random);

    MortonOrdering ordering;
    ordering.build(data.positions(), nullptr);

    const std::span<const MortonKey> keys = ordering.keys();
    REQUIRE(keys.size() == data.size());

    std::vector<bool> seen(data.size(), false);

    for (Index i = 0; i < keys.size(); ++i) {
        CAPTURE(i, keys[i].index);
        REQUIRE(keys[i].index < data.size());
        REQUIRE(!seen[keys[i].index]);
        seen[keys[i].index] = true;

        // The code recorded is the code of the particle it names, which is the
        // one thing a sort of pairs could silently get wrong.
        REQUIRE(keys[i].code == morton_code(data.positions().get(keys[i].index), ordering.cube()));

        if (i > 0) {
            REQUIRE(keys[i - 1].code <= keys[i].code);
        }
    }
}

TEST_CASE("the ordering does not depend on the threads that produced it", "[property][solvers]") {
    // The sort is a parallel merge sort above a size threshold, so this is the
    // test that the parallel path and the serial one agree. They can only agree
    // if the ordering is total, which is why `MortonKey` breaks ties on the
    // particle index: identical codes are common in a sampled sphere's core,
    // and without the tie-break their relative order would be whatever the
    // merge happened to do.
    //
    // The count is above the threshold at which the parallel path engages, or
    // the test would be comparing the serial path against itself.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 40000}, random);

    MortonOrdering serial;
    serial.build(data.positions(), nullptr);

    WorkStealingExecutor executor{ThreadPool::default_worker_count()};
    MortonOrdering parallel;
    parallel.build(data.positions(), &executor);

    REQUIRE(serial.keys().size() == parallel.keys().size());
    REQUIRE(serial.cube().size == parallel.cube().size);

    for (Index i = 0; i < serial.keys().size(); ++i) {
        CAPTURE(kSeed, i);
        REQUIRE(serial.keys()[i].code == parallel.keys()[i].code);
        REQUIRE(serial.keys()[i].index == parallel.keys()[i].index);
    }
}

TEST_CASE("the ordering puts neighbours in space near each other", "[property][solvers]") {
    // The claim the whole scheme rests on, measured rather than asserted. The
    // mean distance between particles adjacent in the sorted order should be a
    // small fraction of the mean distance between particles adjacent in the
    // order they were sampled in, which is uncorrelated with position.
    RandomSource random{kSeed};
    ParticleData data = make_plummer_sphere(PlummerParameters{.count = 4000}, random);

    MortonOrdering ordering;
    ordering.build(data.positions(), nullptr);

    const auto positions = data.positions();

    Real sampled_distance = 0;
    Real sorted_distance = 0;

    for (Index i = 1; i < data.size(); ++i) {
        sampled_distance += norm(positions.get(i) - positions.get(i - 1));
        sorted_distance += norm(positions.get(ordering.keys()[i].index) -
                                positions.get(ordering.keys()[i - 1].index));
    }

    CAPTURE(kSeed, sampled_distance, sorted_distance);

    // A factor of three is inside what the curve delivers on this
    // configuration, where the measured figure is above four, and far outside
    // anything a broken ordering could reach. The test is that the ordering is
    // spatial at all, not that it is optimal.
    REQUIRE(sorted_distance * 3 < sampled_distance);
}

TEST_CASE("an ordering of nothing is not an error", "[unit][solvers]") {
    const ParticleData data;

    MortonOrdering ordering;
    ordering.build(data.positions(), nullptr);

    REQUIRE(ordering.keys().empty());
}
