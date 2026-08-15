#include "orrery/solvers/cuda_direct_solver.hpp"

#ifdef ORRERY_ENABLE_CUDA

#    include <algorithm>
#    include <cstddef>
#    include <cstdint>
#    include <memory>
#    include <optional>
#    include <span>
#    include <utility>

#    include <cuda_runtime.h>

#    include "orrery/backend/cuda_device.hpp"
#    include "orrery/backend/cuda_memory.hpp"
#    include "orrery/backend/worker_statistics.hpp"
#    include "orrery/core/softening.hpp"
#    include "orrery/core/types.hpp"
#    include "orrery/core/vec3_span.hpp"
#    include "orrery/solvers/cuda_kernels.hpp"

namespace orrery::solvers {

using backend::Clock;
using core::Index;
using core::Real;
using core::Vec3Span;

namespace {

/// The largest block the device will accept, capped and rounded down to a power
/// of two.
///
/// The same three constraints `choose_tile_size` weighs for the SYCL kernel, in
/// this vendor's vocabulary, and the smallest wins. The device states a maximum
/// threads per block. The tile has to fit four arrays of `Real` into a block's
/// share of shared memory, since positions and masses are staged together. And
/// there is a practical ceiling: past a few hundred threads a block stops fitting
/// comfortably in a multiprocessor's register file, and occupancy falls faster
/// than the extra reuse gains.
///
/// A power of two because the padded launch is a multiple of the block, and a
/// block that is a multiple of the warp width avoids a partly populated warp on
/// every block.
[[nodiscard]] Index choose_block_size(const backend::CudaDeviceDescription& device) noexcept {
    constexpr Index kCeiling = 256;
    constexpr Index kArraysStaged = 4; // x, y, z, mass

    Index block = std::min<Index>(device.max_threads_per_block, kCeiling);

    if (device.shared_memory_bytes_per_block > 0) {
        const Index by_shared_memory = static_cast<Index>(device.shared_memory_bytes_per_block) /
                                       (kArraysStaged * sizeof(Real));
        block = std::min(block, by_shared_memory);
    }

    Index power = 1;
    while (power * 2 <= block) {
        power *= 2;
    }
    return power;
}

} // namespace

/// Everything that requires the CUDA runtime's header, kept out of the public
/// interface.
///
/// The constructor's parameters are not named after the members they fill,
/// which the SYCL solvers' equivalents are. That is not a style preference: this
/// translation unit is compiled by whichever compiler the build was already
/// using, so it meets the project's full warning set, and GCC's -Wshadow
/// reports a constructor parameter that shadows a member. The SYCL sources are
/// only ever compiled by icpx, which does not, so the question has never come up
/// there. Holding this backend's host halves to the same diagnostics as the rest
/// of the project is the point of keeping them out of .cu files.
struct CudaDirectSolver::Impl {
    Impl(backend::CudaDeviceDescription discovered, core::Softening requested)
        : description(std::move(discovered)),
          softening(requested),
          block(choose_block_size(description)) {}

    backend::CudaDeviceDescription description;
    core::Softening softening;
    Index block;

    InteractionCount count;
    CudaEvaluationTimings timings;

    /// How many particles the arrays below were sized for.
    ///
    /// Kept so that a run at fixed particle count allocates once rather than on
    /// every timestep. A simulation calls `evaluate` millions of times with the
    /// same length, and `cudaMalloc` is a driver call that synchronises the
    /// device, which is a far heavier thing to do per timestep than the USM
    /// allocation the other backend avoids for the same reason.
    Index capacity{0};

    /// The four source arrays in one allocation and the three result arrays in
    /// another, rather than seven allocations.
    ///
    /// This is the one place the CUDA solver's shape differs from the SYCL
    /// solver's for a reason that is not the memory model, and it is worth
    /// stating. There, seven separate shared allocations cost nothing to read
    /// because the kernel dereferenced them where they lay. Here each array
    /// would be a transfer of its own, and a transfer has a fixed cost of a few
    /// microseconds before it has moved a byte. At 1024 particles in single
    /// precision the payload is 16 kB and the fixed costs would be most of the
    /// evaluation. One send and one receive instead.
    backend::CudaHostArray<Real> staged_sources;
    backend::CudaHostArray<Real> staged_results;
    backend::CudaArray<Real> device_sources;
    backend::CudaArray<Real> device_results;

    [[nodiscard]] Index padded_count(Index needed) const noexcept {
        return ((needed + block - 1) / block) * block;
    }

    void ensure_capacity(Index needed) {
        if (needed <= capacity) {
            return;
        }

        // Sized to the padded launch rather than to the particle count, so that
        // the trailing threads of the final block read and write inside the
        // allocation instead of being masked at every access. The same
        // arrangement ADR-0027 established for the other backend.
        const Index padded = padded_count(needed);

        staged_sources = backend::CudaHostArray<Real>{4 * padded};
        staged_results = backend::CudaHostArray<Real>{3 * padded};
        device_sources = backend::CudaArray<Real>{4 * padded};
        device_results = backend::CudaArray<Real>{3 * padded};

        capacity = needed;
    }
};

std::unique_ptr<CudaDirectSolver> CudaDirectSolver::try_create(core::Softening softening) {
    const std::optional<backend::CudaDeviceDescription> description =
        backend::discover_cuda_device();
    if (!description.has_value()) {
        return nullptr;
    }

    // Selecting the device is what creates a context on it, and it is where a
    // driver that enumerated a card it cannot actually use fails. Reported as no
    // device, for the reason `backend/cuda_device.hpp` gives: the caller's
    // question is whether there is a device to run on, and every way of
    // answering no is the same answer.
    //
    // Every CUDA device implements both precisions, so unlike the SYCL solvers
    // there is no capability to check here. What differs between them on this
    // vendor is rate rather than support, which is a benchmark's question rather
    // than a constructor's.
    if (cudaSetDevice(0) != cudaSuccess) {
        static_cast<void>(cudaGetLastError());
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(*description, softening);
    return std::unique_ptr<CudaDirectSolver>{new CudaDirectSolver{std::move(impl)}};
}

CudaDirectSolver::CudaDirectSolver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

CudaDirectSolver::~CudaDirectSolver() = default;
CudaDirectSolver::CudaDirectSolver(CudaDirectSolver&&) noexcept = default;
CudaDirectSolver& CudaDirectSolver::operator=(CudaDirectSolver&&) noexcept = default;

core::Softening CudaDirectSolver::softening() const noexcept {
    return impl_->softening;
}

InteractionCount CudaDirectSolver::interaction_count() const noexcept {
    return impl_->count;
}

void CudaDirectSolver::reset_interaction_count() noexcept {
    impl_->count = {};
}

const backend::CudaDeviceDescription& CudaDirectSolver::device() const noexcept {
    return impl_->description;
}

Index CudaDirectSolver::block_size() const noexcept {
    return impl_->block;
}

const CudaEvaluationTimings& CudaDirectSolver::timings() const noexcept {
    return impl_->timings;
}

bool CudaDirectSolver::uses_device_memory() const noexcept {
    if (impl_->device_sources.empty() || impl_->staged_sources.empty()) {
        return false;
    }

    return backend::pointer_kind(impl_->device_sources.data()) ==
               backend::CudaPointerKind::kDevice &&
           backend::pointer_kind(impl_->staged_sources.data()) ==
               backend::CudaPointerKind::kPinnedHost;
}

void CudaDirectSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                                Vec3Span<Real> accelerations) {
    const Index count = positions.size();
    if (count == 0) {
        ++impl_->count.evaluations;
        impl_->timings = {};
        return;
    }

    impl_->ensure_capacity(count);

    const Index padded = impl_->padded_count(count);

    Clock::time_point mark = Clock::now();

    const auto since = [&mark]() noexcept {
        const Clock::time_point now = Clock::now();
        const backend::Duration elapsed = now - mark;
        mark = now;
        return elapsed;
    };

    Real* const staged = impl_->staged_sources.data();
    Real* const staged_x = staged;
    Real* const staged_y = staged + padded;
    Real* const staged_z = staged + (2 * padded);
    Real* const staged_mass = staged + (3 * padded);

    std::copy_n(positions.x.data(), count, staged_x);
    std::copy_n(positions.y.data(), count, staged_y);
    std::copy_n(positions.z.data(), count, staged_z);
    std::copy_n(masses.data(), count, staged_mass);

    // The padded tail is zeroed rather than left as it was. The kernel masks it
    // out of the physics by index, so these values never reach a sum, but they
    // are still loaded into shared memory on every pass and reading
    // uninitialised memory is undefined however little the result is used.
    std::fill(staged_x + count, staged_x + padded, Real{0});
    std::fill(staged_y + count, staged_y + padded, Real{0});
    std::fill(staged_z + count, staged_z + padded, Real{0});
    std::fill(staged_mass + count, staged_mass + padded, Real{0});

    impl_->timings.staging_in = since();

    // One send rather than four, for the reason `Impl` gives.
    impl_->device_sources.copy_from_host(staged, 4 * padded);
    impl_->timings.transfer_in = since();

    Real* const results = impl_->device_results.data();

    const CudaDirectArguments arguments{.position_x = impl_->device_sources.data(),
                                        .position_y = impl_->device_sources.data() + padded,
                                        .position_z = impl_->device_sources.data() + (2 * padded),
                                        .mass = impl_->device_sources.data() + (3 * padded),
                                        .acceleration_x = results,
                                        .acceleration_y = results + padded,
                                        .acceleration_z = results + (2 * padded),
                                        .count = static_cast<std::uint32_t>(count),
                                        .padded = static_cast<std::uint32_t>(padded),
                                        .softening = impl_->softening};

    backend::check_cuda(launch_cuda_direct(arguments, static_cast<unsigned>(impl_->block)),
                        "the direct summation kernel");
    impl_->timings.kernel = since();

    impl_->device_results.copy_to_host(impl_->staged_results.data(), 3 * padded);
    impl_->timings.transfer_out = since();

    const Real* const result_x = impl_->staged_results.data();
    std::copy_n(result_x, count, accelerations.x.data());
    std::copy_n(result_x + padded, count, accelerations.y.data());
    std::copy_n(result_x + (2 * padded), count, accelerations.z.data());

    impl_->timings.staging_out = since();

    ++impl_->count.evaluations;

    // N(N-1), the same closed form the CPU and SYCL solvers report, so that the
    // three are comparable. The kernel evaluates the padded count squared and
    // masks the difference, and counting what the hardware issued rather than
    // what the physics required would make a row of this table incomparable with
    // every other row in it.
    if (count > 1) {
        const auto pairs = static_cast<std::uint64_t>(count);
        impl_->count.particle_particle += pairs * (pairs - 1);
    }
}

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_CUDA
