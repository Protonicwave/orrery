#include "orrery/solvers/tree_walk.hpp"

#include <span>

#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/direct_kernel.hpp"
#include "orrery/solvers/octree.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Softening;
using core::Vec3;
using core::Vec3Span;

Vec3 walk_tree(const Octree& tree, Vec3Span<const Real> positions, std::span<const Real> masses,
               Index target, Softening softening, AccumulateRange accumulate,
               WalkCounts& counts) noexcept {
    const std::span<const TreeNode> nodes = tree.nodes();

    if (nodes.empty()) {
        // A tree with no nodes is a tree over no particles, so there is no
        // valid target to read a position for either. The check is here rather
        // than folded into the loop condition below for exactly that reason:
        // the read would happen before the loop discovered it had nothing to
        // do, and would be out of bounds.
        return Vec3{};
    }

    const std::span<const Quadrupole> quadrupoles = tree.quadrupoles();
    const bool quadrupole = !quadrupoles.empty();

    const Vec3 position = positions.get(target);

    Vec3 acceleration{};
    Index node = 0;

    while (node < nodes.size()) {
        const TreeNode& cell = nodes[node];
        const Vec3 offset = cell.centre_of_mass - position;

        if (core::squared_norm(offset) >= cell.acceptance_radius_squared) {
            acceleration += monopole_acceleration(offset, cell.mass, softening);

            if (quadrupole) {
                acceleration += quadrupole_acceleration(offset, quadrupoles[node], softening);
            }

            ++counts.particle_cell;
            node = cell.next;
        } else if (cell.leaf()) {
            const Index begin = cell.first_particle;
            const Index end = begin + cell.particle_count;

            if (target >= begin && target < end) {
                // The leaf this particle lives in, summed as the two ranges
                // either side of it. Phase 5 chose this shape so that no branch
                // is needed inside the loop to skip the self term, and Phase 7
                // vectorised the loop on that basis; the tree inherits both
                // without restating either.
                acceleration += accumulate(positions, masses, position, begin, target, softening);
                acceleration += accumulate(positions, masses, position, target + 1, end, softening);
                counts.particle_particle += cell.particle_count - 1;
            } else {
                acceleration += accumulate(positions, masses, position, begin, end, softening);
                counts.particle_particle += cell.particle_count;
            }

            node = cell.next;
        } else {
            // Not far enough away and not a leaf, so the answer is somewhere
            // below. The first child is the next node in the array, which is
            // the whole reason the array is in this order.
            ++node;
        }
    }

    return acceleration;
}

} // namespace orrery::solvers
