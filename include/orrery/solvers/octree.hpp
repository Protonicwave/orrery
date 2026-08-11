#pragma once

/// \file
/// The tree the Barnes-Hut solver walks, and the moments it carries.
///
/// A cell of the tree stands in for the particles inside it. If the cell is far
/// enough from the particle being accelerated, the sum over its contents is
/// replaced by a single term formed from the total mass and the centre of mass,
/// and the N interactions the cell would have contributed become one. That is
/// the whole of the Barnes-Hut idea; everything in this file is the bookkeeping
/// that makes it exact enough to be worth doing and fast enough to be worth
/// building.
///
/// ## The shape in memory
///
/// One array of nodes, in the order a depth-first walk visits them, and no
/// pointers anywhere. Two consequences follow and both are the reason for the
/// choice.
///
/// The first child of a node is the node immediately after it, so descending
/// costs an increment and reads a cache line the walk has usually already
/// fetched. The second is the escape index in `TreeNode::next`: the position of
/// the node a walk should continue at when it does *not* descend into this one,
/// which is the end of this node's subtree. A walk is then a loop over one
/// index with no stack, no recursion and no parent links, and the two branches
/// it can take are `++node` and `node = next`.
///
/// The array is also in an order that suits the machine. The particles were
/// sorted along a space-filling curve before the tree was built
/// (`solvers/morton.hpp`), so consecutive targets walk nearly the same nodes,
/// and those nodes are near each other in this array because the array is in
/// tree order and the tree is in space order.
///
/// ## Nodes are held as structs, and the particles are not
///
/// ADR-0004 stores particle components in separate arrays because the force
/// kernel reads positions and masses and never touches velocities, so an
/// interleaved layout would spend bandwidth on data it does not use. The
/// argument does not carry over to nodes, and this file deliberately does the
/// opposite: a walk arriving at a node reads its centre of mass, its mass, its
/// acceptance radius and one of its two index fields, which is all of it. Held
/// as separate arrays those five reads would touch five cache lines instead of
/// one. The layout follows the access pattern in both cases, which is the
/// decision ADR-0004 was actually making.
///
/// The quadrupole moments are the exception, and they are in an array of their
/// own for the same reason. They are read only when they are switched on, and
/// they are as large as the rest of a node put together, so carrying them
/// inside the node would make the common configuration pay for the rare one on
/// every node it touches.
///
/// ## What the tree does not do
///
/// It is not a spatial index, it answers no queries and it does not survive a
/// force evaluation. Positions change every step, so the tree is rebuilt every
/// step; ADR-0022 records why that is cheaper than maintaining one.
///
/// It also holds no particles. A leaf records a range of the sorted arrays, and
/// the summation over that range is done by the direct kernel of Phase 7,
/// unchanged. The tree's job is to make those ranges short and few.

#include <cstdint>
#include <span>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/morton.hpp"

namespace orrery::solvers {

/// The traceless second moment of a cell about its centre of mass.
///
///     Q_ij = sum_k m_k (3 x_i x_j - |x|^2 delta_ij)
///
/// where x is a particle's displacement from the centre of mass. This is the
/// standard normalisation, in which the potential of the cell at displacement d
/// carries the term `Q_ij d_i d_j / (2 |d|^5)` with no further factors, and the
/// monopole term is unaffected by its presence.
///
/// The tensor is symmetric, so six components describe it, and it is traceless,
/// so five would. The sixth is kept because the walk contracts the tensor with
/// a vector twice per accepted cell, and recovering `zz` from the other two
/// would put an addition inside the hottest loop in the solver to save eight
/// bytes in an array that is only allocated when quadrupoles are asked for.
/// The redundancy is asserted by test rather than assumed.
struct Quadrupole {
    core::Real xx{};
    core::Real xy{};
    core::Real xz{};
    core::Real yy{};
    core::Real yz{};
    core::Real zz{};
};

/// One cell of the tree.
///
/// A leaf holds a non-empty range of the sorted particle arrays. An internal
/// node holds none, and `particle_count` being zero is what distinguishes the
/// two: a cell with no particles in it is never created, so an empty range can
/// carry the meaning without a flag of its own.
struct TreeNode {
    /// The total mass of everything below this node.
    core::Real mass{};

    /// The mass-weighted mean position of everything below this node.
    core::Vec3 centre_of_mass;

    /// The squared distance from the centre of mass beyond which this node may
    /// be treated as a single mass.
    ///
    /// Precomputed at build time, where it costs one division per node, rather
    /// than derived in the walk from a cell size and an opening angle, where it
    /// would cost the same division per node *visit*. The walk then compares
    /// one squared distance against one stored number and needs no arithmetic
    /// to decide.
    ///
    /// Infinite when the opening angle is zero, which is the correct reading of
    /// that request: no cell is ever far enough away, every walk descends to
    /// the leaves, and the solver computes exact direct summation by a slower
    /// route. That case is the validation instrument of
    /// `tests/solvers/barnes_hut_test.cpp` rather than a configuration anyone
    /// would run.
    core::Real acceptance_radius_squared{};

    /// The index just past this node's subtree, which is where a walk that does
    /// not descend into it continues.
    core::Index next{};

    /// The first particle of a leaf, in the sorted order.
    core::Index first_particle{};

    /// How many particles a leaf holds, and zero for an internal node.
    core::Index particle_count{};

    [[nodiscard]] constexpr bool leaf() const noexcept { return particle_count > 0; }
};

/// The choices that decide what the tree costs and how accurate it is.
///
/// They live with the tree rather than with the solver because all three are
/// spent during construction: the opening angle is folded into every node's
/// acceptance radius, the leaf capacity decides where subdivision stops, and
/// the quadrupole switch decides whether the moments are computed at all.
struct TreeParameters {
    /// The opening angle, in the sense of the criterion in `octree.hpp`'s
    /// acceptance radius: a cell of size s is accepted at distance d when
    /// `d >= s / theta + delta`, where delta is the offset of its centre of
    /// mass from its geometric centre.
    ///
    /// Half a radian is the value the literature settles on and the value this
    /// project defaults to, and `docs/performance/barnes_hut.md` measures the
    /// error and the cost either side of it rather than citing anyone.
    ///
    /// Restricted to `[0, 1]`. The upper bound is not a matter of taste: above
    /// one, a cell can be accepted while the particle being accelerated is
    /// still inside it, which would have the particle attract itself through
    /// its own cell's centre of mass. A larger value is reduced to one rather
    /// than rejected, on the same terms as `DirectSolver::select_kernel`, and
    /// the solver reports what it settled on.
    core::Real opening_angle{static_cast<core::Real>(0.5)};

    /// The most particles a cell may hold and still be a leaf.
    ///
    /// The trade is between two costs that move in opposite directions. Small
    /// leaves mean more nodes to build, more nodes to walk and a deeper tree;
    /// large leaves mean more pairs computed directly at the bottom of every
    /// walk. The optimum is not a property of the algorithm but of how fast the
    /// machine computes a pair against how fast it walks a node, and on this
    /// machine the pair is computed four at a time by the AVX2 kernel of Phase
    /// 7, which pushes the answer higher than the eight or sixteen a scalar
    /// implementation would choose.
    ///
    /// Thirty-two is measured rather than assumed, in
    /// `docs/performance/barnes_hut.md`. Values below one are treated as one.
    core::Index leaf_capacity{32};

    /// Whether to compute and use the quadrupole moment of each cell.
    ///
    /// Off by default. It is an accuracy option rather than an improvement:
    /// it costs memory per node, arithmetic per accepted cell and a second
    /// aggregation during construction, and it buys about an order of magnitude
    /// of accuracy at a fixed opening angle. Whether that is a good trade
    /// depends on whether the same accuracy is cheaper to buy by opening the
    /// angle instead, which is a question about the configuration and the
    /// machine and is measured in `docs/performance/barnes_hut.md` rather than
    /// settled here. ADR-0024 records the reasoning.
    bool quadrupole{false};
};

/// `parameters` with the corrections its own documentation describes: the
/// opening angle brought into `[0, 1]` and the leaf capacity to at least one.
///
/// A free function because two places apply it, the tree when it is built and
/// the solver when it is constructed, and a project that wrote the same clamp
/// twice would eventually clamp differently in the two of them.
[[nodiscard]] TreeParameters corrected_parameters(TreeParameters parameters) noexcept;

/// The octree of one configuration, built from Morton-sorted particles.
///
/// Rebuilt in place: a solver holds one of these for its lifetime and calls
/// `build` on every force evaluation, so the node array is allocated on the
/// first evaluation and reused by every later one. The tree of a configuration
/// depends only on the positions and the parameters, and not on how many
/// threads built it.
class Octree {
public:
    /// Build the tree over particles already sorted by Morton code.
    ///
    /// `positions`, `masses` and `keys` describe the same particles in the same
    /// sorted order, and `cube` is the cube those codes were computed in. The
    /// solver above holds the permuted copies and passes them here; nothing in
    /// this class reorders anything.
    ///
    /// Runs the per-subtree work through `executor` where one is given.
    ///
    /// Safe to call with no particles, which produces a tree with no nodes.
    void build(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
               std::span<const MortonKey> keys, const BoundingCube& cube,
               const TreeParameters& parameters, backend::Executor* executor);

    /// The nodes, in depth-first order. The root is the first, where there is
    /// one.
    [[nodiscard]] std::span<const TreeNode> nodes() const noexcept { return nodes_; }

    /// The quadrupole moment of each node, or an empty span when the moments
    /// were not asked for.
    ///
    /// Indexed by node, so `quadrupoles()[n]` belongs to `nodes()[n]`.
    [[nodiscard]] std::span<const Quadrupole> quadrupoles() const noexcept { return quadrupoles_; }

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }

    /// The parameters the last build used, after the corrections described on
    /// `TreeParameters`.
    [[nodiscard]] const TreeParameters& parameters() const noexcept { return parameters_; }

    /// How many nodes are leaves.
    ///
    /// Reported because the ratio of leaves to particles is the first thing to
    /// look at when a tree is slower than it should be: a tree whose leaves
    /// average two particles is paying for nodes it could have avoided.
    [[nodiscard]] core::Index leaf_count() const noexcept { return leaf_count_; }

    /// The depth of the deepest leaf, counting the root as depth zero.
    [[nodiscard]] unsigned depth() const noexcept { return depth_; }

private:
    /// A subtree waiting to be built, and where its root will end up.
    ///
    /// Construction splits into a serial descent that stops at ranges small
    /// enough to be worth handing out, and a parallel pass that builds each of
    /// those ranges into a buffer of its own. This is the handover between
    /// them. `top_index` is the position of the placeholder node in the partial
    /// array the descent produced, which the assembly pass replaces with the
    /// subtree.
    struct Subtree {
        core::Index begin{};
        core::Index end{};
        core::Index top_index{};
        unsigned level{};
        core::Vec3 centre;
        core::Real size{};
        std::vector<TreeNode> nodes;
        std::vector<Quadrupole> quadrupoles;

        /// What this subtree contributes to the whole tree's figures.
        ///
        /// Accumulated here rather than in the members of the tree because
        /// several subtrees are built at once, and two workers incrementing one
        /// counter is a data race whatever the counter is for. They are
        /// combined when the subtrees are spliced in, which is serial.
        core::Index leaves{};
        unsigned depth{};
    };

    /// Everything the recursive builder needs and none of it mutable state of
    /// the tree, so that several of them can run at once.
    struct BuildInput {
        core::Vec3Span<const core::Real> positions;
        std::span<const core::Real> masses;
        std::span<const MortonKey> keys;
    };

    /// The geometry of a node above the subtrees.
    ///
    /// The acceptance radius needs the cell's side and the distance from its
    /// centre to its centre of mass, and a node above the subtrees does not
    /// know its centre of mass until they have been built. So the geometry is
    /// kept beside the partial array until the moments arrive, rather than
    /// stored in every node of the finished tree where the walk would carry it
    /// through the cache and never read it.
    struct TopCell {
        core::Vec3 centre;
        core::Real size{};
    };

    /// Emit the nodes above the subtrees, stopping at ranges of at most
    /// `threshold` particles, and record each stopping point in `subtrees_`.
    void descend(const BuildInput& input, core::Index begin, core::Index end, unsigned level,
                 core::Vec3 centre, core::Real size, core::Index threshold);

    /// Build one subtree into its own buffers, moments and all.
    void build_subtree(const BuildInput& input, Subtree& subtree) const;

    /// Emit one node into `sink`, and unless it is a leaf everything below it.
    ///
    /// The node's own position is the size of the sink's array on entry and its
    /// escape index is the size on exit, which is what makes the escape index
    /// free: it is where the builder had got to when the subtree finished.
    ///
    /// `level` is the absolute level of the cell, counting the root of the
    /// whole tree as zero, because that is what selects the three bits of the
    /// Morton code the split uses.
    void emit(const BuildInput& input, Subtree& sink, core::Index begin, core::Index end,
              unsigned level, core::Vec3 centre, core::Real size) const;

    /// The subtree the descent is about to hand out, reusing the buffers of the
    /// one that occupied this slot in the previous build.
    [[nodiscard]] Subtree& next_subtree();

    /// The acceptance radius of a cell of side `size` whose centre of mass is
    /// `offset` from its geometric centre.
    [[nodiscard]] core::Real acceptance_radius_squared(core::Real size,
                                                       core::Real offset) const noexcept;

    /// Splice the subtrees into the nodes the descent produced.
    void assemble();

    std::vector<TreeNode> nodes_;
    std::vector<Quadrupole> quadrupoles_;

    // The five members below hold nothing between builds except their storage,
    // which is the point of them. A solver evaluates millions of times on
    // configurations of one size, so a tree that allocated its working buffers
    // on every build would spend a measurable part of every force evaluation in
    // the allocator and would return the same pages to it immediately
    // afterwards.

    /// The subtrees of the last build. Kept, along with their node buffers,
    /// rather than destroyed, and reused from the front by `next_subtree`.
    std::vector<Subtree> subtrees_;

    /// How many entries of `subtrees_` the current build is using.
    core::Index subtree_count_{};

    /// Where the assembled node array is put together, before being swapped
    /// with `nodes_`. Swapped rather than copied so that both buffers keep
    /// their capacity and the pair alternates between builds.
    std::vector<TreeNode> assembled_;
    std::vector<Quadrupole> assembled_quadrupoles_;

    /// How far each node of the partial array moves when the subtrees are
    /// spliced in, indexed by its position in that array.
    std::vector<core::Index> shift_;

    /// The geometry of each node of the partial array, in step with it.
    std::vector<TopCell> top_cells_;

    TreeParameters parameters_;
    core::Index leaf_count_{};
    unsigned depth_{};
};

} // namespace orrery::solvers
