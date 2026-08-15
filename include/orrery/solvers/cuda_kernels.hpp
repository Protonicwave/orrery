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
/// compiled by another is a arrangement that happens to work rather than one the
/// standard describes. The caller converts, once, at a boundary where this
/// project already permits exceptions.
///
/// Synchronous. There is nothing to overlap the wait with: the integrator cannot
/// advance until the accelerations exist, which is the same argument the SYCL
/// solvers give for an in-order queue.
[[nodiscard]] cudaError_t launch_cuda_direct(const CudaDirectArguments& arguments, unsigned block);

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
