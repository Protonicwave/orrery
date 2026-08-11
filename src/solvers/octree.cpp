#include "orrery/solvers/octree.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/morton.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Vec3;
using core::Vec3Span;

namespace {

/// How many pieces of work to aim for per worker.
///
/// More than one, because the subtrees of an octree over a clustered
/// configuration differ in size by orders of magnitude and a scheduler cannot
/// balance what it is not given a choice about. Not many more, because each
/// piece carries a node buffer of its own and the assembly pass afterwards is
/// serial in their number. Four is the same figure Phase 6 arrived at for the
/// same reason, at a sixteenth of its chunk count because these pieces are much
/// larger.
constexpr Index kSubtreesPerWorker = 4;

/// The centre of one of the eight children of a cell.
///
/// The bit of the octant index that selects each axis follows `morton_code`:
/// the most significant of the three is x, then y, then z. The two files have
/// to agree, and this is the only other place that opens a code up.
[[nodiscard]] Vec3 child_centre(Vec3 centre, Real size, unsigned octant) noexcept {
    const Real quarter = size / 4;

    return Vec3{centre.x + ((octant & 0b100U) != 0 ? quarter : -quarter),
                centre.y + ((octant & 0b010U) != 0 ? quarter : -quarter),
                centre.z + ((octant & 0b001U) != 0 ? quarter : -quarter)};
}

/// The end of the run of particles in `octant` within `[begin, end)`.
///
/// The keys are sorted and every particle in the range shares the code prefix
/// above `level`, so the octant index at this level is non-decreasing across
/// the range and a binary search finds the boundary. The alternative, a linear
/// scan, would visit every particle at every level and turn construction from
/// O(N log N) into the same thing with a much larger constant.
[[nodiscard]] Index octant_end(std::span<const MortonKey> keys, Index begin, Index end,
                               unsigned level, unsigned octant) noexcept {
    const auto first = std::next(keys.begin(), static_cast<std::ptrdiff_t>(begin));
    const auto last = std::next(keys.begin(), static_cast<std::ptrdiff_t>(end));

    const auto boundary = std::partition_point(first, last, [level, octant](const MortonKey& key) {
        return morton_octant(key.code, level) <= octant;
    });

    return static_cast<Index>(std::distance(keys.begin(), boundary));
}

/// Add the moment of a point mass at displacement `d` to `quadrupole`.
///
/// This is the parallel-axis theorem for the traceless second moment. Combining
/// a child's moment into its parent's needs the child's own tensor plus this
/// term for its mass at its offset; the cross terms vanish because a child's
/// particles have zero first moment about its own centre of mass, which is what
/// a centre of mass is.
void add_displaced_moment(Quadrupole& quadrupole, Real mass, Vec3 d) noexcept {
    const Real trace = mass * core::squared_norm(d);

    quadrupole.xx += (3 * mass * d.x * d.x) - trace;
    quadrupole.xy += 3 * mass * d.x * d.y;
    quadrupole.xz += 3 * mass * d.x * d.z;
    quadrupole.yy += (3 * mass * d.y * d.y) - trace;
    quadrupole.yz += 3 * mass * d.y * d.z;
    quadrupole.zz += (3 * mass * d.z * d.z) - trace;
}

/// Fill in the mass, centre of mass and quadrupole of a leaf from its
/// particles.
///
/// `quadrupole` is null where the moments were not asked for, which is the
/// default configuration and the one that must not pay for them.
void set_leaf_moments(Vec3Span<const Real> positions, std::span<const Real> masses, TreeNode& node,
                      Quadrupole* quadrupole) {
    const Index begin = node.first_particle;
    const Index end = begin + node.particle_count;

    Real mass = 0;
    Vec3 weighted{};

    for (Index i = begin; i < end; ++i) {
        const Real particle_mass = masses[i];
        mass += particle_mass;
        weighted += positions.get(i) * particle_mass;
    }

    node.mass = mass;

    if (mass > 0) {
        node.centre_of_mass = weighted / mass;
    } else {
        // A cell whose particles have no mass between them has no centre of
        // mass, and this project's own container zero-fills, so the case
        // arrives from ordinary code rather than from a pathological test. The
        // mean position is used instead. Nothing downstream can see the
        // difference, since every term formed from this node is multiplied by a
        // mass of zero, but a division by zero here would put a NaN in the node
        // and NaN multiplied by zero is still NaN.
        node.centre_of_mass = positions.get(begin);
    }

    if (quadrupole == nullptr) {
        return;
    }

    *quadrupole = Quadrupole{};

    // A second pass, because the moment is taken about a centre of mass the
    // first pass had not finished computing. Accumulating the raw second
    // moments in one pass and shifting them afterwards is the standard way to
    // avoid it and is not used here: it forms differences of large numbers for
    // a cell far from the origin and loses precision exactly where a quadrupole
    // is being asked for to gain some.
    for (Index i = begin; i < end; ++i) {
        add_displaced_moment(*quadrupole, masses[i], positions.get(i) - node.centre_of_mass);
    }
}

/// Combine the moments of the children of the node at `index` into it.
///
/// `nodes` and `quadrupoles` must already hold the children, which in
/// depth-first order means everything from `index + 1` up to the node's escape
/// index.
void gather_moments(std::vector<TreeNode>& nodes, std::vector<Quadrupole>& quadrupoles,
                    Index index) {
    const Index end = nodes[index].next;

    Real mass = 0;
    Vec3 weighted{};
    Vec3 mean{};
    Index children = 0;

    // The children of a node in depth-first order are the node after it, then
    // whatever that one's escape index points at, and so on until the parent's
    // own escape index. No child list is stored because this is the child list.
    for (Index child = index + 1; child < end; child = nodes[child].next) {
        mass += nodes[child].mass;
        weighted += nodes[child].centre_of_mass * nodes[child].mass;
        mean += nodes[child].centre_of_mass;
        ++children;
    }

    nodes[index].mass = mass;
    nodes[index].centre_of_mass =
        mass > 0 ? weighted / mass : mean / static_cast<Real>(children == 0 ? 1 : children);

    if (quadrupoles.empty()) {
        return;
    }

    Quadrupole moment{};

    for (Index child = index + 1; child < end; child = nodes[child].next) {
        const Quadrupole& child_moment = quadrupoles[child];
        moment.xx += child_moment.xx;
        moment.xy += child_moment.xy;
        moment.xz += child_moment.xz;
        moment.yy += child_moment.yy;
        moment.yz += child_moment.yz;
        moment.zz += child_moment.zz;

        add_displaced_moment(moment, nodes[child].mass,
                             nodes[child].centre_of_mass - nodes[index].centre_of_mass);
    }

    quadrupoles[index] = moment;
}

} // namespace

TreeParameters corrected_parameters(TreeParameters parameters) noexcept {
    parameters.opening_angle = std::clamp(parameters.opening_angle, Real{0}, Real{1});
    parameters.leaf_capacity = std::max<Index>(parameters.leaf_capacity, 1);

    return parameters;
}

void Octree::build(Vec3Span<const Real> positions, std::span<const Real> masses,
                   std::span<const MortonKey> keys, const BoundingCube& cube,
                   const TreeParameters& parameters, backend::Executor* executor) {
    // Corrected here rather than at the call site so that every route to a tree
    // gets it. What was asked for is not kept: a caller that wants to know what
    // it got asks the tree, which is the same contract
    // `DirectSolver::select_kernel` offers.
    parameters_ = corrected_parameters(parameters);

    nodes_.clear();
    quadrupoles_.clear();
    top_cells_.clear();
    subtree_count_ = 0;
    leaf_count_ = 0;
    depth_ = 0;

    const Index count = keys.size();
    if (count == 0) {
        return;
    }

    const BuildInput input{.positions = positions, .masses = masses, .keys = keys};

    // How small a range has to be before it is handed out whole. With one
    // worker the answer is the entire configuration, which makes the descent
    // below emit a single placeholder and the pass after it build the whole
    // tree in one piece. That is the serial path, and it is the same code.
    const unsigned workers = executor == nullptr ? 1 : executor->worker_count();
    const Index threshold =
        workers < 2 ? count
                    : std::max(parameters_.leaf_capacity,
                               count / (kSubtreesPerWorker * static_cast<Index>(workers)));

    descend(input, 0, count, 0, cube.centre(), cube.size, threshold);

    if (executor != nullptr && subtree_count_ > 1) {
        executor->run(subtree_count_, [&](Index begin, Index end) {
            for (Index subtree = begin; subtree < end; ++subtree) {
                build_subtree(input, subtrees_[subtree]);
            }
        });
    } else {
        for (Index subtree = 0; subtree < subtree_count_; ++subtree) {
            build_subtree(input, subtrees_[subtree]);
        }
    }

    assemble();
}

Octree::Subtree& Octree::next_subtree() {
    if (subtree_count_ == subtrees_.size()) {
        subtrees_.emplace_back();
    }

    Subtree& subtree = subtrees_[subtree_count_];
    ++subtree_count_;

    // Cleared rather than assigned, because clearing a vector keeps the storage
    // it has already grown into and this one was sized by the previous build of
    // a configuration of the same size.
    subtree.nodes.clear();
    subtree.quadrupoles.clear();
    subtree.leaves = 0;
    subtree.depth = 0;

    return subtree;
}

// Recursive, and so is `emit` below. Both descend an octree, whose depth is
// bounded at 21 levels by the width of a Morton code, so the stack cost is a
// fixed 21 frames rather than something that grows with the configuration. The
// iterative forms would carry an explicit stack of the same depth and would say
// less clearly what they do; the traversal that runs billions of times, in
// `tree_walk.cpp`, has no recursion and no stack at all, which is where the
// property matters.
// NOLINTNEXTLINE(misc-no-recursion)
void Octree::descend(const BuildInput& input, Index begin, Index end, unsigned level, Vec3 centre,
                     Real size, Index threshold) {
    if (begin >= end) {
        // A cell with nothing in it is not a node. An octree over a clustered
        // configuration is mostly empty space, and materialising the empty
        // cells would cost more nodes than the occupied ones and give the walk
        // eight children to examine where it now has two or three.
        return;
    }

    const Index self = nodes_.size();
    nodes_.emplace_back();
    top_cells_.push_back(TopCell{.centre = centre, .size = size});

    if (end - begin <= threshold || level >= kMortonBitsPerAxis) {
        Subtree& subtree = next_subtree();
        subtree.begin = begin;
        subtree.end = end;
        subtree.top_index = self;
        subtree.level = level;
        subtree.centre = centre;
        subtree.size = size;

        // One slot for now. The assembly pass replaces this placeholder with
        // the subtree that grows from it, and every index after it moves.
        nodes_[self].next = self + 1;
        return;
    }

    const unsigned child_level = level + 1;
    Index child_begin = begin;

    for (unsigned octant = 0; octant < 8; ++octant) {
        const Index child_end = octant_end(input.keys, child_begin, end, child_level, octant);
        descend(input, child_begin, child_end, child_level, child_centre(centre, size, octant),
                size / 2, threshold);
        child_begin = child_end;
    }

    nodes_[self].next = nodes_.size();
}

void Octree::build_subtree(const BuildInput& input, Subtree& subtree) const {
    emit(input, subtree, subtree.begin, subtree.end, subtree.level, subtree.centre, subtree.size);
}

// NOLINTNEXTLINE(misc-no-recursion)
void Octree::emit(const BuildInput& input, Subtree& sink, Index begin, Index end, unsigned level,
                  Vec3 centre, Real size) const {
    const Index self = sink.nodes.size();
    sink.nodes.emplace_back();

    if (parameters_.quadrupole) {
        sink.quadrupoles.emplace_back();
    }

    // The depth limit is not a tuning parameter. Particles at identical
    // positions share a Morton code exactly, so no level separates them, and a
    // builder that subdivided until every cell held few enough particles would
    // not terminate on a configuration containing two of them.
    const bool leaf = (end - begin <= parameters_.leaf_capacity) || level >= kMortonBitsPerAxis;

    if (leaf) {
        sink.nodes[self].first_particle = begin;
        sink.nodes[self].particle_count = end - begin;
        set_leaf_moments(input.positions, input.masses, sink.nodes[self],
                         parameters_.quadrupole ? &sink.quadrupoles[self] : nullptr);

        ++sink.leaves;
        sink.depth = std::max(sink.depth, level);
        sink.nodes[self].next = sink.nodes.size();
    } else {
        const unsigned child_level = level + 1;
        Index child_begin = begin;

        for (unsigned octant = 0; octant < 8; ++octant) {
            const Index child_end = octant_end(input.keys, child_begin, end, child_level, octant);

            if (child_end > child_begin) {
                emit(input, sink, child_begin, child_end, child_level,
                     child_centre(centre, size, octant), size / 2);
            }

            child_begin = child_end;
        }

        // Set before the moments are gathered, because that is how the gather
        // finds where this node's children end. Every recursive call above has
        // returned, so the sink holds exactly this node and its subtree beyond
        // `self`.
        sink.nodes[self].next = sink.nodes.size();
        gather_moments(sink.nodes, sink.quadrupoles, self);
    }

    // The geometry of the criterion, spent once here so that the walk spends
    // nothing. `size` is the side of the cell and the offset is how far its
    // centre of mass has drifted from its geometric centre.
    const Real offset = core::norm(sink.nodes[self].centre_of_mass - centre);
    sink.nodes[self].acceptance_radius_squared = acceptance_radius_squared(size, offset);
}

Real Octree::acceptance_radius_squared(Real size, Real offset) const noexcept {
    if (!(parameters_.opening_angle > 0)) {
        // An opening angle of zero means no cell is ever accepted, and an
        // infinite acceptance radius is that statement in the units the walk
        // compares against. It is a real value the comparison handles rather
        // than a special case the walk has to test for, which is why the walk
        // has no branch for it.
        return std::numeric_limits<Real>::infinity();
    }

    const Real radius = (size / parameters_.opening_angle) + offset;

    return radius * radius;
}

void Octree::assemble() {
    const Index top_count = nodes_.size();

    // How far each node moves. A placeholder occupying one slot becomes a
    // subtree of k nodes, so everything after it moves along by k - 1, and the
    // running total of those is the shift at each position. One entry past the
    // end, because an escape index may point at the end of the array.
    shift_.assign(top_count + 1, 0);

    for (Index subtree = 0; subtree < subtree_count_; ++subtree) {
        shift_[subtrees_[subtree].top_index + 1] += subtrees_[subtree].nodes.size() - 1;
    }

    for (Index top = 1; top <= top_count; ++top) {
        shift_[top] += shift_[top - 1];
    }

    assembled_.assign(top_count + shift_[top_count], TreeNode{});
    assembled_quadrupoles_.assign(parameters_.quadrupole ? assembled_.size() : 0, Quadrupole{});

    // The subtrees were recorded in the order the descent reached them, which
    // is increasing order of position, so one cursor over them keeps step with
    // the walk over the partial array.
    Index subtree = 0;

    for (Index top = 0; top < top_count; ++top) {
        const Index base = top + shift_[top];

        if (subtree < subtree_count_ && subtrees_[subtree].top_index == top) {
            const Subtree& source = subtrees_[subtree];

            for (Index node = 0; node < source.nodes.size(); ++node) {
                assembled_[base + node] = source.nodes[node];
                assembled_[base + node].next += base;

                if (parameters_.quadrupole) {
                    assembled_quadrupoles_[base + node] = source.quadrupoles[node];
                }
            }

            leaf_count_ += source.leaves;
            depth_ = std::max(depth_, source.depth);
            ++subtree;
        } else {
            assembled_[base] = nodes_[top];
            assembled_[base].next = nodes_[top].next + shift_[nodes_[top].next];
        }
    }

    nodes_.swap(assembled_);
    quadrupoles_.swap(assembled_quadrupoles_);

    // The moments of the nodes above the subtrees, which could not be computed
    // until the subtrees they aggregate existed. In reverse order, because a
    // node's children are always after it, so working backwards means every
    // child is finished before its parent is reached. There are a few hundred
    // of these at most against the many thousands inside the subtrees, which is
    // why this pass is serial and the one that produced them is not.
    //
    // The cursor walks the subtrees backwards in step with the loop, for the
    // same reason the one above walks them forwards: their positions are
    // increasing, so a comparison against the next one either way is enough to
    // recognise a root that already has its moments.
    Index root = subtree_count_;

    for (Index top = top_count; top > 0; --top) {
        const Index node = (top - 1) + shift_[top - 1];

        if (root > 0 && subtrees_[root - 1].top_index == top - 1) {
            --root;
            continue;
        }

        gather_moments(nodes_, quadrupoles_, node);

        const Real offset = core::norm(nodes_[node].centre_of_mass - top_cells_[top - 1].centre);
        nodes_[node].acceptance_radius_squared =
            acceptance_radius_squared(top_cells_[top - 1].size, offset);
    }
}

} // namespace orrery::solvers
