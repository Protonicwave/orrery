#include "orrery/solvers/octree.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/core/particle_data.hpp"
#include "orrery/core/random.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/initial_conditions/plummer.hpp"
#include "orrery/solvers/morton.hpp"

namespace {

using orrery::backend::Executor;
using orrery::backend::ThreadPool;
using orrery::backend::WorkStealingExecutor;
using orrery::core::Index;
using orrery::core::norm;
using orrery::core::ParticleData;
using orrery::core::RandomSource;
using orrery::core::Real;
using orrery::core::squared_norm;
using orrery::core::Vec3;
using orrery::initial_conditions::make_plummer_sphere;
using orrery::initial_conditions::PlummerParameters;
using orrery::solvers::MortonOrdering;
using orrery::solvers::Octree;
using orrery::solvers::Quadrupole;
using orrery::solvers::TreeNode;
using orrery::solvers::TreeParameters;

constexpr std::uint64_t kSeed = 20260810;

/// A tree over a configuration, with the ordering it was built from.
///
/// The two are always used together, and a test that built one without the
/// other would be testing a tree over particles in an order it did not expect.
struct BuiltTree {
    MortonOrdering ordering;
    Octree tree;

    /// The particles in the tree's order, which is what the node ranges index.
    ParticleData sorted;
};

[[nodiscard]] BuiltTree build(const ParticleData& data, const TreeParameters& parameters,
                              Executor* executor = nullptr) {
    BuiltTree built;
    built.ordering.build(data.positions(), executor);

    built.sorted.resize(data.size());

    for (Index i = 0; i < data.size(); ++i) {
        const Index source = built.ordering.keys()[i].index;
        built.sorted.positions().set(i, data.positions().get(source));
        built.sorted.masses()[i] = data.masses()[source];
    }

    built.tree.build(built.sorted.positions(), built.sorted.masses(), built.ordering.keys(),
                     built.ordering.cube(), parameters, executor);

    return built;
}

[[nodiscard]] ParticleData sampled_sphere(Index count, RandomSource& random) {
    return make_plummer_sphere(PlummerParameters{.count = count}, random);
}

/// The tolerance a sum of `count` terms deserves in the build's precision.
[[nodiscard]] Real summation_tolerance(Index count) {
    const Real epsilon = std::numeric_limits<Real>::epsilon();

    return 8 * epsilon * static_cast<Real>(count);
}

} // namespace

TEST_CASE("an empty configuration produces no nodes", "[unit][solvers]") {
    const ParticleData data;
    const BuiltTree built = build(data, TreeParameters{});

    REQUIRE(built.tree.empty());
    REQUIRE(built.tree.nodes().empty());
    REQUIRE(built.tree.leaf_count() == 0);
}

TEST_CASE("a configuration inside one leaf is a single node", "[unit][solvers]") {
    // Below the leaf capacity there is nothing to subdivide, and a tree that
    // built internal nodes anyway would make the walk descend through cells
    // holding one child each to reach the particles it could have summed at
    // the root.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{2});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{3});

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 8});

    REQUIRE(built.tree.nodes().size() == 1);
    REQUIRE(built.tree.leaf_count() == 1);
    REQUIRE(built.tree.depth() == 0);

    const TreeNode& root = built.tree.nodes()[0];
    REQUIRE(root.leaf());
    REQUIRE(root.particle_count == 2);
    REQUIRE(root.first_particle == 0);
    REQUIRE(root.next == 1);
    REQUIRE(root.mass == Real{5});

    // Two masses of 2 and 3 at -1 and +1 put the centre of mass at 1/5, which
    // is exact in binary only as the quotient it is written as here.
    REQUIRE(root.centre_of_mass.x == Real{1} / Real{5});
    REQUIRE(root.centre_of_mass.y == Real{0});
    REQUIRE(root.centre_of_mass.z == Real{0});
}

TEST_CASE("the leaves partition the particles exactly once each", "[property][solvers]") {
    // The invariant every other property of the tree rests on. A particle in no
    // leaf never attracts anything and a particle in two leaves attracts
    // everything twice, and both would leave the total mass at the root
    // looking correct.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(2000, random);

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 8});

    std::vector<int> visits(data.size(), 0);
    Index leaves = 0;

    for (const TreeNode& node : built.tree.nodes()) {
        if (!node.leaf()) {
            continue;
        }

        ++leaves;
        REQUIRE(node.particle_count <= 8);

        for (Index i = node.first_particle; i < node.first_particle + node.particle_count; ++i) {
            REQUIRE(i < data.size());
            ++visits[i];
        }
    }

    REQUIRE(leaves == built.tree.leaf_count());

    for (Index i = 0; i < data.size(); ++i) {
        CAPTURE(i);
        REQUIRE(visits[i] == 1);
    }
}

TEST_CASE("the escape indices describe the subtrees they claim to", "[property][solvers]") {
    // The walk has no stack and no parent pointers, so an escape index that
    // pointed anywhere but past the end of its own subtree would send a
    // traversal into a sibling's children or past a cell it should have
    // examined, and the answer would be wrong rather than slow.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(2000, random);

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 8});
    const std::span<const TreeNode> nodes = built.tree.nodes();

    for (Index index = 0; index < nodes.size(); ++index) {
        CAPTURE(index);

        // Strictly forwards, and never past the end.
        REQUIRE(nodes[index].next > index);
        REQUIRE(nodes[index].next <= nodes.size());

        if (nodes[index].leaf()) {
            REQUIRE(nodes[index].next == index + 1);
            continue;
        }

        // An internal node's children start at the node after it, are reached
        // by following escape indices, and end exactly at its own. The particles
        // of the children are contiguous and cover no more than the parent's own
        // span, which is what makes a leaf's range a range at all.
        Index children = 0;
        Index child = index + 1;

        while (child < nodes[index].next) {
            REQUIRE(nodes[child].next <= nodes[index].next);
            child = nodes[child].next;
            ++children;
        }

        REQUIRE(child == nodes[index].next);
        REQUIRE(children >= 1);
        REQUIRE(children <= 8);
    }
}

TEST_CASE("every node carries the mass and centre of mass of its own particles",
          "[property][solvers]") {
    // Computed here from the particles a node claims, by a route that has
    // nothing in common with the aggregation the builder used: the builder
    // combines children into parents and this sums the leaves' particles
    // directly. If the parallel-axis bookkeeping were wrong the two would part
    // company at the first internal node.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(1500, random);

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 8});
    const std::span<const TreeNode> nodes = built.tree.nodes();

    const auto positions = built.sorted.positions();
    const auto masses = built.sorted.masses();

    for (Index index = 0; index < nodes.size(); ++index) {
        // The particles below a node are those of the leaves in its subtree,
        // which in depth-first order is a contiguous range of node indices.
        Real mass = 0;
        Vec3 weighted{};
        Index particles = 0;

        for (Index node = index; node < nodes[index].next; ++node) {
            if (!nodes[node].leaf()) {
                continue;
            }

            for (Index i = nodes[node].first_particle;
                 i < nodes[node].first_particle + nodes[node].particle_count; ++i) {
                mass += masses[i];
                weighted += positions.get(i) * masses[i];
                ++particles;
            }
        }

        const Vec3 centre = weighted / mass;
        const Real tolerance = summation_tolerance(particles);

        CAPTURE(index, particles, mass, nodes[index].mass);

        REQUIRE(std::abs(nodes[index].mass - mass) <= tolerance * mass);
        REQUIRE(norm(nodes[index].centre_of_mass - centre) <= tolerance * (norm(centre) + 1));
    }
}

TEST_CASE("the quadrupole moment is the second moment about the centre of mass",
          "[property][solvers]") {
    // The moment of the root, computed here in one pass over every particle
    // against the builder's accumulation up the tree. The two agree only if the
    // parallel-axis theorem is applied with the right sign, the right factor of
    // three and the right trace, none of which a smaller test would separate.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(1000, random);

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 8, .quadrupole = true});

    REQUIRE(built.tree.quadrupoles().size() == built.tree.nodes().size());

    const TreeNode& root = built.tree.nodes()[0];
    const Quadrupole& moment = built.tree.quadrupoles()[0];

    const auto positions = built.sorted.positions();
    const auto masses = built.sorted.masses();

    Quadrupole expected{};

    for (Index i = 0; i < data.size(); ++i) {
        const Vec3 d = positions.get(i) - root.centre_of_mass;
        const Real trace = masses[i] * squared_norm(d);

        expected.xx += (3 * masses[i] * d.x * d.x) - trace;
        expected.xy += 3 * masses[i] * d.x * d.y;
        expected.xz += 3 * masses[i] * d.x * d.z;
        expected.yy += (3 * masses[i] * d.y * d.y) - trace;
        expected.yz += 3 * masses[i] * d.y * d.z;
        expected.zz += (3 * masses[i] * d.z * d.z) - trace;
    }

    // The scale to judge the difference against. The components of a quadrupole
    // straddle zero, so a relative comparison per component would divide by
    // something arbitrarily small; the size of the tensor is the right
    // denominator.
    const Real scale = std::abs(expected.xx) + std::abs(expected.yy) + std::abs(expected.zz) +
                       std::abs(expected.xy) + std::abs(expected.xz) + std::abs(expected.yz);
    const Real tolerance = summation_tolerance(data.size()) * scale;

    CAPTURE(kSeed, scale, tolerance);

    REQUIRE(std::abs(moment.xx - expected.xx) <= tolerance);
    REQUIRE(std::abs(moment.xy - expected.xy) <= tolerance);
    REQUIRE(std::abs(moment.xz - expected.xz) <= tolerance);
    REQUIRE(std::abs(moment.yy - expected.yy) <= tolerance);
    REQUIRE(std::abs(moment.yz - expected.yz) <= tolerance);
    REQUIRE(std::abs(moment.zz - expected.zz) <= tolerance);

    // And the property that makes five components enough to describe six: the
    // tensor is traceless by construction. The sixth is stored anyway, and this
    // is the assertion that keeps the redundancy honest.
    REQUIRE(std::abs(moment.xx + moment.yy + moment.zz) <= tolerance);
}

TEST_CASE("a symmetric configuration has no quadrupole moment", "[unit][solvers]") {
    // Eight equal masses at the corners of a cube about the origin. The
    // distribution is symmetric under every reflection that could produce a
    // moment, so the tensor is exactly zero, and each of the six components is
    // a sum of eight terms that cancel in pairs rather than a small residue.
    ParticleData data;

    for (int corner = 0; corner < 8; ++corner) {
        data.add(Vec3{(corner & 0b100) != 0 ? Real{1} : Real{-1},
                      (corner & 0b010) != 0 ? Real{1} : Real{-1},
                      (corner & 0b001) != 0 ? Real{1} : Real{-1}},
                 Vec3{}, Real{1});
    }

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 16, .quadrupole = true});

    const Quadrupole& moment = built.tree.quadrupoles()[0];
    const Real tolerance = summation_tolerance(data.size()) * 8;

    CAPTURE(moment.xx, moment.yy, moment.zz, moment.xy, moment.xz, moment.yz);

    REQUIRE(std::abs(moment.xx) <= tolerance);
    REQUIRE(std::abs(moment.yy) <= tolerance);
    REQUIRE(std::abs(moment.zz) <= tolerance);
    REQUIRE(std::abs(moment.xy) <= tolerance);
    REQUIRE(std::abs(moment.xz) <= tolerance);
    REQUIRE(std::abs(moment.yz) <= tolerance);
}

TEST_CASE("the quadrupole moments are not computed unless they are asked for", "[unit][solvers]") {
    // The default configuration must not pay for them. They are as large as the
    // rest of a node put together, and a walk that carried them through the
    // cache without reading them would be slower than one that could not.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(200, random);

    const BuiltTree built = build(data, TreeParameters{});

    REQUIRE(built.tree.quadrupoles().empty());
    REQUIRE(!built.tree.nodes().empty());
}

TEST_CASE("the acceptance radius follows the opening angle and the offset", "[unit][solvers]") {
    // Two particles at opposite corners of the cube they define, with unequal
    // masses so that the centre of mass is not the centre of the cell. The root
    // is then a cell whose radius is the sum of both terms of the criterion,
    // which is the case the classical form would get wrong.
    ParticleData data;
    data.add(Vec3{-1, 0, 0}, Vec3{}, Real{3});
    data.add(Vec3{1, 0, 0}, Vec3{}, Real{1});

    const Real angle = static_cast<Real>(0.5);
    const BuiltTree built = build(data, TreeParameters{.opening_angle = angle, .leaf_capacity = 8});

    const TreeNode& root = built.tree.nodes()[0];

    // The cube is 2 on a side and centred at the origin. The centre of mass of
    // 3 at -1 and 1 at +1 is at -1/2, so the offset is 1/2.
    const Real expected = (2 / angle) + Real{0.5};

    CAPTURE(root.acceptance_radius_squared, expected * expected);

    REQUIRE(std::abs(root.acceptance_radius_squared - (expected * expected)) <=
            summation_tolerance(4) * expected * expected);
}

TEST_CASE("an opening angle of zero accepts nothing", "[unit][solvers]") {
    // The request is that no cell ever stands in for its contents, and the tree
    // expresses it as an infinite acceptance radius so that the walk needs no
    // branch for the case. Infinity rather than a large finite number, because
    // a finite one would be a distance a configuration could exceed.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(100, random);

    const BuiltTree built = build(data, TreeParameters{.opening_angle = 0, .leaf_capacity = 8});

    for (const TreeNode& node : built.tree.nodes()) {
        REQUIRE(std::isinf(node.acceptance_radius_squared));
    }
}

TEST_CASE("the parameters are corrected rather than trusted", "[unit][solvers]") {
    // An opening angle above one would let a cell be accepted while the
    // particle being accelerated was still inside it, which is a particle
    // attracting itself. A leaf capacity of zero would ask for a subdivision
    // that never terminates. Both are reduced to the nearest usable value and
    // reported, on the same terms as a request for a vector kernel the machine
    // cannot run.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(100, random);

    const BuiltTree built = build(data, TreeParameters{.opening_angle = 5, .leaf_capacity = 0});

    REQUIRE(built.tree.parameters().opening_angle == Real{1});
    REQUIRE(built.tree.parameters().leaf_capacity == 1);

    const BuiltTree negative = build(data, TreeParameters{.opening_angle = static_cast<Real>(-1)});

    REQUIRE(negative.tree.parameters().opening_angle == Real{0});
}

TEST_CASE("particles at one point terminate at the depth limit", "[unit][solvers]") {
    // Identical positions share a Morton code exactly, so no level separates
    // them and the subdivision cannot reduce the count. The depth limit is what
    // makes the builder terminate, and the resulting leaf holds more particles
    // than the capacity asked for, which is correct: they cannot be told apart.
    ParticleData data;

    for (int i = 0; i < 40; ++i) {
        data.add(Vec3{2, -1, 3}, Vec3{}, Real{1});
    }

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 4});

    REQUIRE(!built.tree.nodes().empty());
    REQUIRE(built.tree.leaf_count() == 1);

    const TreeNode& leaf = built.tree.nodes()[built.tree.nodes().size() - 1];
    REQUIRE(leaf.leaf());
    REQUIRE(leaf.particle_count == 40);
    REQUIRE(leaf.mass == Real{40});

    // And the radius is finite and positive, so a particle inside the cell
    // opens it rather than being accelerated by a cell it belongs to.
    REQUIRE(leaf.acceptance_radius_squared > Real{0});
    REQUIRE(!std::isinf(leaf.acceptance_radius_squared));
}

TEST_CASE("particles of no mass do not produce a tree of NaN", "[unit][solvers]") {
    // A container that has been sized but not filled holds particles of zero
    // mass at the origin, which this project's own storage produces and an
    // initial-condition generator hands over halfway through building. A centre
    // of mass computed by dividing by the total would be a NaN, and a NaN
    // multiplied by the zero mass that produced it is still a NaN.
    const ParticleData data{16};

    const BuiltTree built = build(data, TreeParameters{.leaf_capacity = 4, .quadrupole = true});

    for (const TreeNode& node : built.tree.nodes()) {
        REQUIRE(node.mass == Real{0});
        REQUIRE(std::isfinite(node.centre_of_mass.x));
        REQUIRE(std::isfinite(node.centre_of_mass.y));
        REQUIRE(std::isfinite(node.centre_of_mass.z));
        REQUIRE(std::isfinite(node.acceptance_radius_squared));
    }
}

TEST_CASE("the tree does not depend on the threads that built it", "[property][solvers]") {
    // Construction divides into subtrees that are built independently and
    // spliced together afterwards, so this is the test that the splice is
    // arithmetic rather than luck. Node for node, index for index: a tree that
    // differed in one escape index would give a different answer for one
    // particle and would be very hard to find later.
    //
    // The count is large enough that the descent produces many subtrees rather
    // than handing the whole configuration to one worker.
    RandomSource random{kSeed};
    const ParticleData data = sampled_sphere(60000, random);

    const TreeParameters parameters{.leaf_capacity = 16, .quadrupole = true};

    WorkStealingExecutor executor{ThreadPool::default_worker_count()};

    const BuiltTree serial = build(data, parameters);
    const BuiltTree parallel = build(data, parameters, &executor);

    REQUIRE(serial.tree.nodes().size() == parallel.tree.nodes().size());
    REQUIRE(serial.tree.leaf_count() == parallel.tree.leaf_count());
    REQUIRE(serial.tree.depth() == parallel.tree.depth());
    REQUIRE(serial.tree.nodes().size() > 1000);

    for (Index index = 0; index < serial.tree.nodes().size(); ++index) {
        const TreeNode& one = serial.tree.nodes()[index];
        const TreeNode& other = parallel.tree.nodes()[index];

        CAPTURE(index, one.next, other.next);

        REQUIRE(one.next == other.next);
        REQUIRE(one.first_particle == other.first_particle);
        REQUIRE(one.particle_count == other.particle_count);
        REQUIRE(one.mass == other.mass);
        REQUIRE(one.centre_of_mass == other.centre_of_mass);
        REQUIRE(one.acceptance_radius_squared == other.acceptance_radius_squared);
        REQUIRE(serial.tree.quadrupoles()[index].xx == parallel.tree.quadrupoles()[index].xx);
        REQUIRE(serial.tree.quadrupoles()[index].yz == parallel.tree.quadrupoles()[index].yz);
    }
}

TEST_CASE("a tree is rebuilt rather than accumulated", "[unit][solvers]") {
    // The solver above builds into one tree on every force evaluation, so a
    // build that appended to what it found would grow without bound and would
    // walk nodes describing where the particles used to be. The buffers are
    // reused; nothing in them is.
    RandomSource random{kSeed};
    const ParticleData large = sampled_sphere(2000, random);
    const ParticleData small = sampled_sphere(50, random);

    const TreeParameters parameters{.leaf_capacity = 8};

    MortonOrdering ordering;
    Octree tree;

    const auto rebuild = [&](const ParticleData& data) {
        ordering.build(data.positions(), nullptr);

        ParticleData sorted{data.size()};
        for (Index i = 0; i < data.size(); ++i) {
            const Index source = ordering.keys()[i].index;
            sorted.positions().set(i, data.positions().get(source));
            sorted.masses()[i] = data.masses()[source];
        }

        tree.build(sorted.positions(), sorted.masses(), ordering.keys(), ordering.cube(),
                   parameters, nullptr);

        return std::pair{tree.nodes().size(), tree.leaf_count()};
    };

    const auto first = rebuild(large);
    const auto second = rebuild(small);
    const auto third = rebuild(large);

    REQUIRE(second.first < first.first);
    REQUIRE(second.second < first.second);

    // The same configuration gives the same tree the third time as the first,
    // which is the statement that nothing survived from the build in between.
    REQUIRE(third == first);
    REQUIRE(first.second > 0);
}
