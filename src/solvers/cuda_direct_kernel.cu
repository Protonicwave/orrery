#include "orrery/solvers/cuda_kernels.hpp"

#ifdef ORRERY_ENABLE_CUDA

#    include <cuda_runtime.h>

#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/units.hpp"

/// \file
/// Direct summation on an NVIDIA GPU.
///
/// The same physics as `solvers/direct_solver.hpp` and the same physics as
/// `solvers/sycl_direct_solver.hpp`, over the same softened potential from
/// `core/softening.hpp`, in the same units. This file is one of the two places
/// in the project the device compiler sees, and it contains a kernel and its
/// launch and nothing else: no allocation, no staging, no timing, no counters.
/// `solvers/cuda_kernels.hpp` explains why the split is drawn there.
///
/// ## The kernel is the SYCL kernel
///
/// That is a claim worth making precisely, because the whole point of writing a
/// second backend is to find out how much of the first one was about gravity and
/// how much was about Intel.
///
/// One thread per target, each accumulating the acceleration on its own particle
/// from every source, so the write pattern is the one ADR-0015 chose in Phase 5:
/// read everything, write only yourself, no reduction between threads and no
/// atomics anywhere. Sources are staged through shared memory a tile at a time,
/// because otherwise every thread in a block reads the same source position from
/// global memory on the same instruction. The self term and the padded tail are
/// masked rather than branched around, and both the mass and the squared
/// separation are selected, never the mass alone, because the reciprocal
/// distance of a zero separation is infinite in an unsoftened run and `inf * 0`
/// is a NaN rather than the zero contribution intended.
///
/// Every sentence of that paragraph is also true of the SYCL kernel, and most of
/// it is a paraphrase of that file's comment. The translation is mechanical:
/// `nd_item::get_global_id` becomes `blockIdx * blockDim + threadIdx`,
/// `local_accessor` becomes shared memory, `group_barrier` becomes
/// `__syncthreads`. Nothing about the algorithm changed, and finding that out is
/// what ADR-0060 says this backend exists for.
///
/// ## What is genuinely different
///
/// Two things, and neither is in this file.
///
/// The tile size is dynamic shared memory rather than a compile-time array,
/// because the block size is chosen from what the device reports and the same
/// binary runs on cards with different limits. SYCL's `local_accessor` is sized
/// at submission for the same reason, so this is the same decision in a
/// different spelling.
///
/// The arrays these pointers address are device memory rather than shared
/// unified memory, so the host cannot read the results where the kernel wrote
/// them. That difference is entirely in the solver above and is the subject of
/// `backend/cuda_memory.hpp`.
///
/// ## What it will and will not agree with
///
/// Not bit for bit with the CPU solver, for the reason the AVX2 and SYCL kernels
/// are not: the sum is reassociated, here tile by tile rather than index by
/// index, and the device is entitled to contract a multiply and an add into a
/// fused multiply-add. Neither is a relaxation of IEEE arithmetic and ADR-0020's
/// rule against fast-math flags is not weakened. The kernels are compiled
/// without `--use_fast_math` precisely because that flag would substitute an
/// approximate reciprocal square root for the one every interaction performs,
/// which is the single operation this project's accuracy figures are most
/// sensitive to.
///
/// Accuracy is quoted against the compensated double-precision reference in
/// `solvers/reference_kernel.hpp`, as every other kernel here is, and
/// `tests/solvers/cuda_direct_solver_test.cpp` states the tolerance and derives
/// it from the precision the build was configured with. The tolerances are the
/// same numbers the SYCL tests use, which is the strongest form the phase's
/// cross-device agreement requirement can take on machines that have one device
/// each.

namespace orrery::solvers {

namespace {

using core::Real;

/// One thread per target, sources staged through shared memory a tile at a time.
__global__ void direct_kernel(const CudaDirectArguments arguments) {
    // Dynamic rather than a fixed-size array, because the tile is the block size
    // and that is chosen from what the device reports. One allocation, divided
    // into the four component arrays the kernel stages, so that the launch asks
    // the driver for one extent rather than four.
    extern __shared__ char staged[];

    const unsigned tile = blockDim.x;

    Real* const tile_x = reinterpret_cast<Real*>(staged);
    Real* const tile_y = tile_x + tile;
    Real* const tile_z = tile_y + tile;
    Real* const tile_mass = tile_z + tile;

    const unsigned target = (blockIdx.x * blockDim.x) + threadIdx.x;
    const unsigned lane = threadIdx.x;

    // Read unconditionally. The arrays are allocated to the padded launch and
    // their tails were zeroed, so a trailing thread reads inside the allocation
    // and its position is never used for anything.
    const Real x = arguments.position_x[target];
    const Real y = arguments.position_y[target];
    const Real z = arguments.position_z[target];

    Real sum_x{0};
    Real sum_y{0};
    Real sum_z{0};

    for (unsigned base = 0; base < arguments.padded; base += tile) {
        // One cooperative load per thread, then the whole block reads the tile
        // out of shared memory. This is the reuse the kernel exists for: each
        // source position is fetched from global memory once per block rather
        // than once per target, which turns the traffic from O(N^2) into
        // O(N^2 / tile).
        const unsigned source = base + lane;
        tile_x[lane] = arguments.position_x[source];
        tile_y[lane] = arguments.position_y[source];
        tile_z[lane] = arguments.position_z[source];
        tile_mass[lane] = arguments.mass[source];

        __syncthreads();

        for (unsigned j = 0; j < tile; ++j) {
            const Real dx = tile_x[j] - x;
            const Real dy = tile_y[j] - y;
            const Real dz = tile_z[j] - z;

            // Two sources contribute nothing and both have to be masked in the
            // separation as well as in the mass.
            //
            // A particle does not attract itself. The CPU kernel excludes that
            // by splitting the source range either side of the target's index,
            // which costs nothing in a loop; a tile is shared by a whole block
            // and each thread has a different index to exclude, so the term is
            // masked here instead.
            //
            // The tail beyond the particle count is padding, which exists so the
            // inner loop can run over whole tiles without a bound check. It
            // carries zero mass.
            //
            // Zeroing the mass is not sufficient for either. The reciprocal
            // distance of a zero separation is infinite in an unsoftened run and
            // `inf * 0` is a NaN rather than the zero contribution intended, so
            // the squared separation is selected to one and the mass does its
            // job on a finite number. Both cases reach zero separation: the self
            // term always, and a padded source whenever a real particle sits at
            // the origin, which is exactly where the central body of a Kepler
            // configuration sits.
            const unsigned source_index = base + j;
            const bool masked = source_index == target || source_index >= arguments.count;

            const Real separation_squared =
                masked ? Real{1} : ((dx * dx) + (dy * dy) + (dz * dz));
            const Real source_mass = masked ? Real{0} : tile_mass[j];

            // The same function the CPU kernel, the SYCL kernel and the
            // potential energy diagnostic call, compiled for a third target.
            // There is one definition of the softened force in this project, and
            // `core/device.hpp` is what lets this compiler see it.
            const Real factor =
                source_mass * core::softened_inverse_distance_cubed(separation_squared,
                                                                    arguments.softening);

            sum_x += dx * factor;
            sum_y += dy * factor;
            sum_z += dz * factor;
        }

        // Before overwriting the tile on the next pass. Without this a fast
        // thread would load the next tile over values a slow one in the same
        // block is still reading.
        __syncthreads();
    }

    // G is one in this project's units (ADR-0007), written for the reason
    // `direct_solver.cpp` gives rather than for its value.
    arguments.acceleration_x[target] = core::kGravitationalConstant * sum_x;
    arguments.acceleration_y[target] = core::kGravitationalConstant * sum_y;
    arguments.acceleration_z[target] = core::kGravitationalConstant * sum_z;
}

} // namespace

cudaError_t launch_cuda_direct(const CudaDirectArguments& arguments, unsigned block) {
    if (arguments.padded == 0 || block == 0) {
        return cudaSuccess;
    }

    const unsigned blocks = arguments.padded / block;

    // Four component arrays of one tile each. Asked for as one extent because
    // that is what the launch interface takes, and divided inside the kernel.
    const unsigned shared = 4U * block * static_cast<unsigned>(sizeof(Real));

    direct_kernel<<<blocks, block, shared>>>(arguments);

    // Two failures, and they arrive by different routes. A launch the driver
    // refuses, because the block is too large or the shared memory request
    // exceeds what the device offers, is reported immediately by
    // `cudaGetLastError`. A fault during execution is not reported until
    // something waits. Both are checked, in that order, because the first would
    // otherwise be attributed to the second and diagnosed as a kernel bug rather
    // than as a launch configuration the device declined.
    const cudaError_t launched = cudaGetLastError();
    if (launched != cudaSuccess) {
        return launched;
    }

    return cudaDeviceSynchronize();
}

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
