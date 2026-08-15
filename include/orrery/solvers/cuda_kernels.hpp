#pragma once

/// \file
/// Everything the device compiler exports, and nothing else.
///
/// This is the whole boundary between the CUDA kernels and the rest of the
/// project, and it is deliberately narrow. Two functions, each taking one
/// trivially copyable bundle of arguments and returning what the runtime said.
/// Nothing above this line knows that a kernel launch has a syntax of its own,
/// and nothing below it knows what a `ForceSolver` is.
///
/// ## Why there is a boundary here at all, when SYCL needed none
///
/// SYCL is one compiler for both halves of the program, so `-fsycl` makes every
/// translation unit a device translation unit and the kernel is written inline
/// in the solver that submits it. That is convenient and it has a price the
/// project has already paid and documented: the address sanitiser cannot be
/// applied to any translation unit compiled that way, so the SYCL solvers are
/// the one part of this repository the sanitiser run does not cover
/// (`docs/performance/sycl_direct.md` records the gap and its shape).
///
/// CUDA splits the two. Only a `.cu` file needs nvcc; a `.cpp` that calls
/// `cudaMalloc` needs the runtime's header and its library. So this backend
/// puts as little as it can on the far side of that split. The two `.cu` files
/// contain the kernels and their launch configuration and nothing else: no
/// allocation, no staging, no timing, no counter arithmetic, no solver. All of
/// that stays in ordinary C++, compiled by the project's own compiler, under the
/// project's own warning set, seen by clang-tidy, and covered by the sanitiser
/// builds.
///
/// The gap that remains is the two kernels themselves, which is smaller than the
/// SYCL gap by every measure and is the smallest this vendor's toolchain allows.
/// ADR-0060 records the decision.
///
/// ## Why arguments are bundled rather than passed
///
/// A kernel launch copies its parameters into a fixed and small region of
/// constant memory, and a launch that overruns it fails at run time rather than
/// at compile time. Bundling makes the size of that copy one thing to look at.
/// It is also the same shape `WalkArguments` takes in the SYCL tree solver, for
/// the same reason: two kernels take the same fourteen pointers and a list of
/// fourteen parameters written twice is a list that will eventually be written
/// differently twice.

#ifdef ORRERY_ENABLE_CUDA

#    include <cstdint>

#    include <cuda_runtime.h>

#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3.hpp"
#    include "orrery/solvers/octree.hpp"

namespace orrery::solvers {

/// What the direct summation kernel reads and writes.
///
/// Raw pointers, which is the one place section 4 of the implementation plan
/// permits them over spans, and this is that place twice over: the pointers
/// address device memory the host cannot dereference, and a `std::span` would
/// carry a length the kernel never reads while not being a device pointer in any
/// sense the type system could check.
struct CudaDirectArguments {
    const core::Real* position_x{};
    const core::Real* position_y{};
    const core::Real* position_z{};
    const core::Real* mass{};

    core::Real* acceleration_x{};
    core::Real* acceleration_y{};
    core::Real* acceleration_z{};

    /// How many particles there really are.
    ///
    /// The kernel runs over `padded` targets and masks the difference, so this
    /// is what decides which sources carry mass. Thirty-two bits because a
    /// configuration with more than four billion particles would need more
    /// memory than any card carries by three orders of magnitude, and because
    /// halving the index arithmetic in the innermost loop is worth having.
    std::uint32_t count{};

    /// The launch size, which is `count` rounded up to a whole number of blocks.
    ///
    /// The arrays are allocated to this rather than to `count`, so the trailing
    /// threads of the final block read and write inside the allocation instead
    /// of being masked at every access. The masking on the physics stays; this
    /// removes it from the addressing. The same arrangement ADR-0027 established
    /// for the other backend.
    std::uint32_t padded{};

    core::Softening softening;
};

/// Launch direct summation and wait for it.
///
/// `block` threads per block, which is also the number of sources staged through
/// shared memory per pass, so it decides both the occupancy and the reuse. The
/// caller chooses it from what the device reports and reports what it chose.
///
/// Returns the runtime's status rather than throwing, because an exception
/// thrown inside a translation unit compiled by one compiler and caught in one
/// compiled by another is an arrangement that happens to work rather than one the
/// standard describes. The caller converts, once, at a boundary where this
/// project already permits exceptions.
///
/// Synchronous. There is nothing to overlap the wait with: the integrator cannot
/// advance until the accelerations exist, which is the same argument the SYCL
/// solvers give for an in-order queue.
[[nodiscard]] cudaError_t launch_cuda_direct(const CudaDirectArguments& arguments, unsigned block);

/// One cell of the tree, in the shape the device wants it.
///
/// The same narrowing `sycl_tree_solver.cpp` performs, for the same two reasons
/// and with the same measurements behind them, which is a result rather than a
/// coincidence: ADR-0030 argued the layout from cache line sizes and index
/// ranges, and neither of those is a property of a vendor.
///
/// The indices become 32-bit. `core::Index` is `std::size_t` for the reason
/// `core/types.hpp` gives, which is a statement about the host's containers and
/// buys nothing here: this configuration would need four billion particles
/// before a 32-bit node index could overflow. Halving the three index fields
/// takes a node in a single-precision build from 48 bytes to 32, which is
/// exactly a quarter of this device's 128-byte cache line, so four nodes share
/// one line and a warp reading one node reads one aligned block.
///
/// The quadrupoles stay in an array of their own, for the reason `octree.hpp`
/// gives: they are read only when they are switched on, and carrying them inside
/// the node would double what every walk pulls through the cache in the
/// configuration that does not use them.
///
/// Declared here rather than in the solver because both sides of the device
/// boundary construct it: the host fills the array and the kernel reads it, and
/// a layout described twice is a layout that will eventually be described
/// differently twice.
struct CudaTreeNode {
    core::Vec3 centre_of_mass;
    core::Real mass{};
    core::Real acceptance_radius_squared{};

    std::uint32_t next{};
    std::uint32_t first_particle{};
    std::uint32_t particle_count{};
};

/// Which of the two traversals the device runs.
///
/// Both are kept and both are correct, for the reason `TreeTraversal` in
/// `solvers/sycl_tree_solver.hpp` gives: the independent walk is the baseline
/// the coherent walk is measured against, and a mitigation whose baseline has
/// been deleted is a mitigation nobody can check.
///
/// A separate enumeration from that one rather than a shared type. They name the
/// same two ideas, and a caller holding a solver of one kind cannot hand its
/// setting to the other, so sharing the type would only offer a conversion
/// nobody should perform.
enum class CudaTraversal : std::uint8_t {
    /// One thread per target, each following its own node index.
    kIndependent,

    /// One node index per group of lanes, advanced by agreement.
    kCoherent,
};

/// What the tree traversal reads and writes.
///
/// Bundled for the reason `CudaDirectArguments` is, and more so: there are
/// fourteen pointers here and two kernels take the same fourteen.
struct CudaTreeArguments {
    const CudaTreeNode* nodes{};

    /// Null when the moments are switched off, which is the whole of the test
    /// the kernel makes. The branch is uniform across every thread in the
    /// launch, so it predicts perfectly and costs one register.
    const Quadrupole* quadrupoles{};

    const core::Real* position_x{};
    const core::Real* position_y{};
    const core::Real* position_z{};
    const core::Real* mass{};

    core::Real* acceleration_x{};
    core::Real* acceleration_y{};
    core::Real* acceleration_z{};

    /// Per target, reduced on the host. A counter shared between threads would
    /// be an atomic written billions of times per evaluation and the most
    /// contended address on the device, which is the same argument `WalkCounts`
    /// makes for the CPU walk and `WalkArguments` for the SYCL one.
    std::uint32_t* pair_counts{};
    std::uint32_t* cell_counts{};
    std::uint32_t* visit_counts{};

    std::uint32_t node_count{};
    std::uint32_t count{};

    core::Softening softening;
};

/// Launch the tree traversal and wait for it.
///
/// `width` is how many lanes agree to walk together, and it is where this
/// backend's traversal differs from the SYCL one in more than spelling.
///
/// There, the width is the sub-group size and is requested through a kernel
/// attribute, so the device chooses whether it can offer 8, 16 or 32 and a
/// request it cannot meet falls back to the compiler's own choice. Here the warp
/// is 32 lanes and nothing can change that. So a narrower width is implemented
/// rather than requested: the reduction that advances the node index is taken
/// over segments of the warp instead of over the whole of it, which makes 8 and
/// 16 mean what they meant on the other device even though the hardware is
/// executing 32 lanes either way.
///
/// The two are therefore not the same measurement, and
/// `docs/performance/cuda.md` says so where it reports the sweep. On Intel a
/// narrow sub-group is narrower hardware; here it is the same hardware told to
/// agree in smaller groups. What survives the difference is the quantity the
/// sweep exists to expose, which is how much redundant walking coherence costs
/// as the group grows.
///
/// Only 8, 16 and 32 are implemented, because each is a separate instantiation
/// and the set is the same one the SYCL solver offers. Anything else is served
/// at the warp width.
///
/// Returns the runtime's status rather than throwing, for the reason
/// `launch_cuda_direct` gives. Synchronous, for the same reason again.
[[nodiscard]] cudaError_t launch_cuda_tree(const CudaTreeArguments& arguments,
                                           CudaTraversal traversal, unsigned block, unsigned width);

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
