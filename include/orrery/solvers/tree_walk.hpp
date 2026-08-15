#pragma once

/// \file
/// The traversal that turns a tree into an acceleration, which is where a
/// Barnes-Hut solver spends nearly all of its time.
///
/// The whole of the algorithm is here, in one loop. Starting at the root, a
/// node is either far enough away to stand in for its contents, in which case
/// one term is added and the walk skips its subtree, or it is not, in which
/// case the walk descends into it. A leaf that is not far enough away is summed
/// directly, pair by pair, by the kernel Phase 7 vectorised.
///
/// ## No stack
///
/// The obvious traversal is recursive, or keeps an explicit stack of nodes
/// still to visit. This one keeps neither, because the tree was laid out to
/// make both unnecessary (`solvers/octree.hpp`). Nodes are stored in the order
/// a depth-first walk meets them, so the first child of a node is the node
/// immediately after it, and each node records the index just past its own
/// subtree. Descending is `++node`. Skipping is `node = next`. There is no
/// third case.
///
/// That matters for more than tidiness. A recursive walk touches a call frame
/// per level and returns through every one of them; this one is a loop over a
/// single index whose two branches both move forwards through an array that is
/// already in the order it is read. The hardware prefetcher can follow it,
/// which is the sense in which this traversal respects the memory hierarchy.
///
/// ## The criterion
///
/// A cell is accepted when the target is at least its acceptance radius from
/// its centre of mass. That radius is
///
///     s / theta + delta
///
/// where s is the cell's side, theta the opening angle and delta the distance
/// from the cell's geometric centre to its centre of mass. The first term is
/// the classical Barnes-Hut criterion. The second is the correction that makes
/// it safe, and ADR-0023 sets out the failure it prevents: a cell whose mass
/// has collected in one corner has a centre of mass far from its geometry, and
/// the classical test measures the distance from a point the mass is not at.
///
/// The radius is precomputed per node, so this file compares one squared
/// distance against one stored number and does no arithmetic to decide.
///
/// ## What the walk guarantees
///
/// The order of summation depends only on the tree, so two evaluations of the
/// same configuration agree bit for bit however many threads ran them. Each
/// target is summed over the nodes in index order, and nothing about which
/// worker holds which target changes that. This is the property that lets the
/// tree solver be compared against direct summation at all, and
/// `tests/solvers/barnes_hut_test.cpp` asserts it rather than assuming it.
///
/// A particle never contributes to its own acceleration. The opening angle
/// cannot exceed one (`TreeParameters::opening_angle`), which is exactly the
/// condition under which a cell containing the target is always opened, so the
/// only place a target can meet itself is the leaf it lives in, and there it is
/// skipped by summing the ranges either side of it. That is the same device the
/// direct solver uses, for the same reason: no branch in the innermost loop.
///
/// ## Why the two multipole terms are defined here rather than compiled away
///
/// `monopole_acceleration` and `quadrupole_acceleration` are `inline` in this
/// header rather than out of line beside `walk_tree`, and that is deliberate
/// rather than a matter of where the compiler can see them. The GPU traversal of
/// Phase 10 (`solvers/sycl_tree_solver.hpp`) runs a different loop over the same
/// tree, and it calls exactly these two functions, compiled for the device. That
/// is the property single-source SYCL buys and the reason ADR-0025 weighs it
/// above the alternatives: the multipole expansion of a cell is written once in
/// this project, not once per backend.
///
/// What the two traversals do not share is the loop, because the loop is the
/// whole subject of Phase 10. ADR-0029 records the shape the device needs and
/// why it cannot be the shape below.

#include <cstdint>
#include <span>

#include "orrery/core/device.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/octree.hpp"

namespace orrery::solvers {

/// What one walk did, in the two units `InteractionCount` reports.
///
/// Returned by the walk rather than accumulated into a counter it was handed,
/// because the walks of different targets run on different threads and a shared
/// counter incremented per interaction would be both a data race and, once made
/// atomic, the most contended line in the program.
struct WalkCounts {
    std::uint64_t particle_particle{};
    std::uint64_t particle_cell{};
};

/// The acceleration on the particle at `target`, without the factor of G.
///
/// `positions`, `masses` and `target` are in the tree's own sorted order, which
/// is what the leaves index. `accumulate` is the direct kernel used for the
/// pairs at the leaves, chosen by the caller once per force evaluation rather
/// than here.
///
/// Adds to `counts` rather than replacing it, so that a caller summing a chunk
/// of targets passes one record through all of them.
///
/// An empty tree gives zero, which is the acceleration of a particle with
/// nothing to attract it.
[[nodiscard]] core::Vec3 walk_tree(const Octree& tree, core::Vec3Span<const core::Real> positions,
                                   std::span<const core::Real> masses, core::Index target,
                                   core::Softening softening, AccumulateRange accumulate,
                                   WalkCounts& counts) noexcept;

/// The acceleration at `offset` from a cell of the given mass, without the
/// factor of G.
///
/// `offset` points from the target to the cell's centre of mass, which is the
/// direction the acceleration is in for a positive mass. The result is the
/// gradient of the softened monopole potential and nothing else.
///
/// Exposed for the tests, which check it against a single particle placed at
/// the cell's centre of mass: a cell holding one particle must produce exactly
/// what the direct kernel produces for that particle, or the tree and the
/// reference are not computing the same physics.
[[nodiscard]] ORRERY_DEVICE inline core::Vec3
monopole_acceleration(core::Vec3 offset, core::Real mass, core::Softening softening) noexcept {
    // The same expression as one iteration of the direct kernel, with the
    // cell's total mass in place of a particle's and its centre of mass in
    // place of a position. That is what a monopole is, and writing it as
    // anything else would put a second copy of the force law in the project.
    const core::Real factor =
        mass * core::softened_inverse_distance_cubed(core::squared_norm(offset), softening);

    return offset * factor;
}

/// The correction the quadrupole moment adds to the term above.
///
/// The next term of the multipole expansion of the same potential:
///
///     a = -(Q d) / r^5 + (5/2) (d . Q d) d / r^7
///
/// with d the offset from the target to the centre of mass and r the softened
/// distance. It falls off two powers faster than the monopole term, which is
/// why it buys accuracy at a fixed opening angle and why it is worth nothing at
/// all for a distant cell.
///
/// Softened by the same substitution as the monopole term, which is exact for
/// the monopole and an approximation here. The approximation costs nothing that
/// can be measured: a cell is only accepted when the target is many cell widths
/// away, and the softening length is a fraction of the smallest cell.
[[nodiscard]] ORRERY_DEVICE inline core::Vec3
quadrupole_acceleration(core::Vec3 offset, const Quadrupole& moment,
                        core::Softening softening) noexcept {
    const core::Real inverse =
        core::softened_inverse_distance(core::squared_norm(offset), softening);

    // Formed from the fifth and seventh powers of one reciprocal square root
    // rather than from two further calls, because the divide and square root
    // unit is the bottleneck this project measured in Phase 7 and these are
    // multiplications.
    const core::Real inverse_squared = inverse * inverse;
    const core::Real inverse_fifth = inverse_squared * inverse_squared * inverse;
    const core::Real inverse_seventh = inverse_fifth * inverse_squared;

    // Q d, with the tensor's symmetry spent: six stored components describe
    // nine, and the three off-diagonal ones appear twice each.
    const core::Vec3 contracted{
        (moment.xx * offset.x) + (moment.xy * offset.y) + (moment.xz * offset.z),
        (moment.xy * offset.x) + (moment.yy * offset.y) + (moment.yz * offset.z),
        (moment.xz * offset.x) + (moment.yz * offset.y) + (moment.zz * offset.z)};

    const core::Real quadratic = core::dot(offset, contracted);

    return (contracted * -inverse_fifth) +
           (offset * (static_cast<core::Real>(2.5) * quadratic * inverse_seventh));
}

} // namespace orrery::solvers
