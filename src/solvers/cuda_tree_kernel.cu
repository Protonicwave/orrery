#include "orrery/solvers/cuda_kernels.hpp"

#ifdef ORRERY_ENABLE_CUDA

#    include <cstdint>

#    include <cuda_runtime.h>

#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/units.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/solvers/tree_walk.hpp"

/// \file
/// The Barnes-Hut traversal on an NVIDIA GPU.
///
/// `solvers/sycl_tree_solver.hpp` sets out what makes a tree walk hard on a wide
/// machine and what this project does about it, at length, and none of that
/// argument is repeated here because none of it changed. In short: a GPU
/// executes lanes in fixed-width groups sharing one instruction pointer, two
/// neighbouring particles agree about most of the tree and disagree about the
/// part nearest them, so a walk written as though each lane were a thread costs
/// what the whole group's walks add up to rather than what its own does. The
/// mitigation is to make the group walk together, advancing to the smallest node
/// index any lane still wants, with a lane that has accepted a cell contributing
/// nothing until the group leaves the subtree it accepted.
///
/// That the argument transfers unchanged is the finding, not the assumption.
/// ADR-0029 justified the scheme from how SIMD hardware executes branches, and
/// this file is the evidence that the justification was about SIMD hardware
/// rather than about Intel's.
///
/// ## The three differences
///
/// **The group is a warp segment rather than a sub-group.** SYCL lets a kernel
/// ask for a sub-group of 8, 16 or 32, and the device compiles the kernel for
/// the width it was asked for. A warp is 32 lanes and there is no such request.
/// So a narrower coherence is built rather than requested: the reduction that
/// advances the node index runs over segments of the warp. At width 32 the two
/// are the same thing. Below it they are not, and
/// `docs/performance/cuda.md` reports the sweep as what it is.
///
/// **The reduction is a shuffle rather than a group collective.**
/// `sycl::reduce_over_group` becomes a logarithmic ladder of `__shfl_xor_sync`,
/// which is what that collective compiles to on this hardware anyway. Written
/// out because CUDA has no portable spelling of the collective at sub-warp
/// granularity, and because a reader comparing the two files should be able to
/// see that the same reduction is being performed.
///
/// **The loop condition is asked of the whole warp.** Every lane has to reach
/// every shuffle, or the exchange is undefined. Within one segment the node
/// index is uniform, so a segment finishes all at once; across segments it is
/// not, so a warp iterates until its slowest segment is done and the finished
/// segments contribute the end of the tree and do no work. The SYCL traversal
/// needs no equivalent because a sub-group is the collective's whole domain.
///
/// ## What is not given up
///
/// The answer. A lane that accepts a cell adds the cell's term and then
/// contributes nothing until the group leaves that subtree, so each target is
/// summed over exactly the nodes its own walk would have visited, in the same
/// order. That is what separates this scheme from the published warp-coherent
/// traversals it resembles, which let the lanes that would have accepted a cell
/// descend with the group and so compute a different, more accurate sum whose
/// value depends on which particles shared a warp. ADR-0029 records why that was
/// not acceptable, and the consequence here is the same one it has there: the
/// interaction counts this traversal reports can be required to equal the CPU
/// solver's exactly, which they are.

namespace orrery::solvers {

namespace {

using core::Index;
using core::Real;
using core::Vec3;

/// What one lane accumulates as it walks.
struct LaneState {
    Vec3 acceleration{};
    std::uint32_t pairs{};
    std::uint32_t cells{};
    std::uint32_t visits{};
};

/// The pairs of one leaf, summed into `acceleration`, with the target skipped.
///
/// Masked rather than split into two ranges either side of the target, exactly
/// as the SYCL walk masks it and for the same reason: on a device the two-range
/// form is two loops with different trip counts, which is a second source of
/// divergence inside the one place the lanes were doing identical work.
///
/// Both the mass and the squared separation are selected, never the mass alone,
/// for the reason the direct kernel gives at greater length.
__device__ void accumulate_leaf(const CudaTreeArguments& arguments, Index target, Vec3 position,
                                Index begin, Index end, Vec3& acceleration) {
    for (Index j = begin; j < end; ++j) {
        const Real dx = arguments.position_x[j] - position.x;
        const Real dy = arguments.position_y[j] - position.y;
        const Real dz = arguments.position_z[j] - position.z;

        const bool self = j == target;
        const Real separation_squared = self ? Real{1} : ((dx * dx) + (dy * dy) + (dz * dz));
        const Real source_mass = self ? Real{0} : arguments.mass[j];

        const Real factor = source_mass * core::softened_inverse_distance_cubed(
                                              separation_squared, arguments.softening);

        acceleration.x += dx * factor;
        acceleration.y += dy * factor;
        acceleration.z += dz * factor;
    }
}

/// What this lane does at `node`, and where its own walk would go next.
///
/// The whole of Barnes-Hut, for one target at one cell, factored out of both
/// traversals below. They differ in how the node index advances and in nothing
/// else, which is the claim this function exists to make structural rather than
/// verbal: if the acceptance test or the leaf summation differed between them,
/// comparing their timings would be comparing two algorithms.
[[nodiscard]] __device__ std::uint32_t visit_node(const CudaTreeArguments& arguments, Index target,
                                                  Vec3 position, std::uint32_t node,
                                                  LaneState& state) {
    const CudaTreeNode cell = arguments.nodes[node];
    const Vec3 offset = cell.centre_of_mass - position;

    if (core::squared_norm(offset) >= cell.acceptance_radius_squared) {
        // The same two functions the CPU walk and the SYCL walk call, compiled
        // for a third target. There is one multipole expansion in this project
        // rather than one per backend, and `core/device.hpp` is what lets this
        // compiler see it.
        state.acceleration += monopole_acceleration(offset, cell.mass, arguments.softening);

        if (arguments.quadrupoles != nullptr) {
            state.acceleration +=
                quadrupole_acceleration(offset, arguments.quadrupoles[node], arguments.softening);
        }

        ++state.cells;
        return cell.next;
    }

    if (cell.particle_count > 0) {
        const Index begin = cell.first_particle;
        const Index end = begin + cell.particle_count;

        accumulate_leaf(arguments, target, position, begin, end, state.acceleration);

        // The target's own leaf contributes one pair fewer, since a particle
        // does not attract itself. Counted the way the CPU walk counts it so
        // that the two totals can be required to be equal.
        const bool holds_target = target >= begin && target < end;
        state.pairs += cell.particle_count - (holds_target ? 1U : 0U);

        return cell.next;
    }

    // Not far enough away and not a leaf, so the answer is somewhere below and
    // the first child is the next node in the array.
    return node + 1;
}

__device__ void write_result(const CudaTreeArguments& arguments, Index target,
                             const LaneState& state) {
    // G is one in this project's units (ADR-0007), written for the reason
    // `direct_solver.cpp` gives rather than for its value.
    arguments.acceleration_x[target] = core::kGravitationalConstant * state.acceleration.x;
    arguments.acceleration_y[target] = core::kGravitationalConstant * state.acceleration.y;
    arguments.acceleration_z[target] = core::kGravitationalConstant * state.acceleration.z;

    arguments.pair_counts[target] = state.pairs;
    arguments.cell_counts[target] = state.cells;
    arguments.visit_counts[target] = state.visits;
}

/// One thread per target, each following its own node index.
///
/// The port a first attempt writes, and the baseline the coherent walk is
/// measured against. Nothing here is wrong: it computes the right answer, it
/// keeps no stack, and every branch it takes is the branch the CPU walk takes.
/// What it does not do is acknowledge that its neighbours in the warp are
/// executing the same instruction stream.
__global__ void independent_walk(const CudaTreeArguments arguments) {
    const unsigned target = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (target >= arguments.count) {
        // The launch is padded to whole blocks. There are no collectives in this
        // traversal, so a padded thread has nothing to take part in and leaves.
        return;
    }

    const Vec3 position{arguments.position_x[target], arguments.position_y[target],
                        arguments.position_z[target]};

    LaneState state;
    std::uint32_t node = 0;

    while (node < arguments.node_count) {
        ++state.visits;
        node = visit_node(arguments, target, position, node, state);
    }

    write_result(arguments, target, state);
}

/// The smallest value held by any lane of this thread's `Width`-lane segment.
///
/// `sycl::reduce_over_group` with `sycl::minimum`, written out. The `width`
/// argument to the shuffle divides the warp into independent segments, so at
/// width 32 this is a warp-wide all-reduce and below it there are several
/// running side by side.
///
/// Every lane of the warp must reach every shuffle, which is why the caller
/// keeps the loop running until the slowest segment has finished rather than
/// letting a finished segment leave.
template<unsigned Width>
[[nodiscard]] __device__ std::uint32_t segment_minimum(std::uint32_t value) {
    for (unsigned offset = Width / 2; offset > 0; offset >>= 1) {
        const std::uint32_t other =
            __shfl_xor_sync(0xFFFFFFFFU, value, static_cast<int>(offset), static_cast<int>(Width));
        value = other < value ? other : value;
    }
    return value;
}

/// One node index per segment of the warp, advanced to the nearest node any lane
/// in it still needs.
///
/// The mitigation. Every lane of the segment is at the same node at the same
/// time, so the node is read once for the segment rather than once per lane, the
/// acceptance test is one instruction over `Width` targets, and the loop has a
/// single instruction stream instead of `Width` interleaved ones.
///
/// A lane that has accepted a cell records the index its own walk would have
/// jumped to and contributes nothing until the group reaches it. That is the
/// entire mechanism by which this traversal computes the same sum as the
/// independent one rather than a more accurate one.
///
/// The segment advances to the smallest index any lane still wants, which makes
/// the sequence of visited nodes exactly the union of the lanes' own walks. A
/// vote on whether to descend would be one cheap instruction rather than a
/// ladder, and would sometimes step through a subtree every lane had already
/// accepted; the minimum is taken instead because the union is the figure
/// `node_visits` is supposed to report.
template<unsigned Width> __global__ void coherent_walk(const CudaTreeArguments arguments) {
    const unsigned target = (blockIdx.x * blockDim.x) + threadIdx.x;
    const bool active = target < arguments.count;

    // Read unconditionally. The arrays are allocated to the padded launch and
    // the tail was zeroed at staging time, so a padded lane reads inside the
    // allocation and its position is never used for anything.
    const Vec3 position{arguments.position_x[target], arguments.position_y[target],
                        arguments.position_z[target]};

    LaneState state;

    std::uint32_t node = 0;

    /// The index this lane's own walk jumped to when it accepted a cell. While
    /// the segment is below it, this lane is inside a subtree it has already
    /// summed as a single mass and takes no part.
    std::uint32_t skip_until = 0;

    // Asked of the whole warp rather than of this lane, because every lane has
    // to reach every shuffle below. Within a segment the node index is uniform,
    // so a segment finishes all at once and then contributes the end of the tree
    // to its own reduction while the rest of the warp carries on.
    while (__any_sync(0xFFFFFFFFU, node < arguments.node_count) != 0) {
        std::uint32_t lane_next = arguments.node_count;

        if (node < arguments.node_count) {
            ++state.visits;

            // Where this lane's own walk would go next. A lane that is skipping
            // wants the end of the subtree it accepted; an inactive lane wants
            // the end of the tree and so never holds the segment back.
            if (active && node >= skip_until) {
                lane_next = visit_node(arguments, target, position, node, state);

                // An accepted cell is the only case that jumps past a subtree,
                // and the only case where this lane has to stop looking. A leaf
                // and a descent both move to a node this lane still wants.
                if (lane_next > node + 1) {
                    skip_until = lane_next;
                }
            } else if (active) {
                lane_next = skip_until;
            }
        }

        node = segment_minimum<Width>(lane_next);
    }

    if (active) {
        write_result(arguments, target, state);
    }
}

} // namespace

cudaError_t launch_cuda_tree(const CudaTreeArguments& arguments, CudaTraversal traversal,
                             unsigned block, unsigned width) {
    if (arguments.count == 0 || block == 0) {
        return cudaSuccess;
    }

    const unsigned padded = ((arguments.count + block - 1) / block) * block;
    const unsigned blocks = padded / block;

    if (traversal == CudaTraversal::kIndependent) {
        independent_walk<<<blocks, block>>>(arguments);
    } else {
        // A switch rather than a loop, because the width is a template argument:
        // the shuffle ladder's length is fixed at compile time, so each width is
        // a separate instantiation. The same shape the SYCL solver's dispatch
        // takes, and for the same reason.
        switch (width) {
        case 8:
            coherent_walk<8><<<blocks, block>>>(arguments);
            break;
        case 16:
            coherent_walk<16><<<blocks, block>>>(arguments);
            break;
        default:
            coherent_walk<32><<<blocks, block>>>(arguments);
            break;
        }
    }

    // Two failures by two routes, checked in this order for the reason
    // `launch_cuda_direct` gives: a launch the driver refused would otherwise be
    // attributed to the synchronisation and diagnosed as a fault in the kernel.
    const cudaError_t launched = cudaGetLastError();
    if (launched != cudaSuccess) {
        return launched;
    }

    return cudaDeviceSynchronize();
}

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
