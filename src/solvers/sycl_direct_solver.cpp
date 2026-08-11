#include "orrery/solvers/sycl_direct_solver.hpp"

#ifdef ORRERY_ENABLE_SYCL

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include <sycl/sycl.hpp>

#include "orrery/backend/sycl_device.hpp"
#include "orrery/backend/sycl_usm.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/units.hpp"
#include "orrery/core/vec3_span.hpp"

namespace orrery::solvers {

using core::Index;
using core::Real;
using core::Vec3Span;

namespace {

/// The largest tile the device will accept, capped and rounded down to a power
/// of two.
///
/// Three constraints, and the smallest wins. The device states a maximum
/// work-group size. The tile has to fit four arrays of `Real` into the
/// work-group's share of local memory, since positions and masses are staged
/// together. And there is a practical ceiling: past a few hundred work-items a
/// group stops fitting comfortably in an Xe-core's register file, and occupancy
/// falls faster than the extra reuse gains.
///
/// A power of two because the padded global range is a multiple of the tile, and
/// a tile that divides the sub-group width evenly avoids a partly populated
/// sub-group on every group.
[[nodiscard]] Index choose_tile_size(const backend::DeviceDescription& device) noexcept {
    constexpr Index kCeiling = 256;
    constexpr Index kArraysStaged = 4; // x, y, z, mass

    Index tile = std::min<Index>(device.max_work_group_size, kCeiling);

    if (device.local_memory_bytes > 0) {
        const Index by_local_memory =
            static_cast<Index>(device.local_memory_bytes) / (kArraysStaged * sizeof(Real));
        tile = std::min(tile, by_local_memory);
    }

    // Round down to a power of two. A device reporting a maximum that is not one
    // is unusual but permitted, and the rounding is cheaper than reasoning about
    // what a ragged tile would do to the sub-group.
    Index power = 1;
    while (power * 2 <= tile) {
        power *= 2;
    }
    return power;
}

} // namespace

/// Everything that requires a SYCL header, kept out of the public interface.
struct SyclDirectSolver::Impl {
    explicit Impl(sycl::device device, backend::DeviceDescription description,
                  core::Softening softening)
        : queue(device, sycl::property::queue::in_order{}), description(std::move(description)),
          softening(softening), tile(choose_tile_size(this->description)) {}

    /// In-order because there is exactly one kernel and nothing to overlap it
    /// with. An out-of-order queue would buy the ability to run independent
    /// submissions concurrently, and the integrator cannot proceed until this
    /// one has finished, so it would buy nothing here and cost the event
    /// bookkeeping to prove it.
    sycl::queue queue;

    backend::DeviceDescription description;
    core::Softening softening;
    Index tile;

    InteractionCount count;
    SyclEvaluationTimings timings;

    /// How many particles the arrays below were sized for.
    ///
    /// Kept so that a run at fixed particle count allocates once rather than on
    /// every timestep. A simulation calls `evaluate` millions of times with the
    /// same length, and a USM allocation is a driver call rather than a bump of
    /// a pointer.
    Index capacity{0};

    backend::UsmArray<Real> position_x;
    backend::UsmArray<Real> position_y;
    backend::UsmArray<Real> position_z;
    backend::UsmArray<Real> mass;
    backend::UsmArray<Real> acceleration_x;
    backend::UsmArray<Real> acceleration_y;
    backend::UsmArray<Real> acceleration_z;

    void ensure_capacity(Index count_needed) {
        if (count_needed <= capacity) {
            return;
        }

        // Sized to the padded range the kernel launches over rather than to the
        // particle count, so that the tail work-items of the final group read
        // and write inside the allocation instead of being masked at every
        // access. The masking on the physics stays; this removes it from the
        // addressing.
        const Index padded = ((count_needed + tile - 1) / tile) * tile;

        position_x = backend::UsmArray<Real>{queue, padded};
        position_y = backend::UsmArray<Real>{queue, padded};
        position_z = backend::UsmArray<Real>{queue, padded};
        mass = backend::UsmArray<Real>{queue, padded};
        acceleration_x = backend::UsmArray<Real>{queue, padded};
        acceleration_y = backend::UsmArray<Real>{queue, padded};
        acceleration_z = backend::UsmArray<Real>{queue, padded};

        capacity = count_needed;
    }
};

std::unique_ptr<SyclDirectSolver> SyclDirectSolver::try_create(core::Softening softening) {
    const std::optional<backend::DeviceDescription> description = backend::discover_gpu_device();
    if (!description.has_value()) {
        return nullptr;
    }

    // A device that cannot run this build's scalar type is not a device this
    // solver can use. The target GPU has no fp64, so a double-precision build
    // declines here and the caller falls back to the CPU, which is the honest
    // outcome rather than a kernel that runs at a fiftieth of the device's
    // single-precision rate through emulation.
    if (!description->supports_build_precision() || !description->supports_shared_usm) {
        return nullptr;
    }

    try {
        const std::vector<sycl::device> devices =
            sycl::device::get_devices(sycl::info::device_type::gpu);
        if (devices.empty()) {
            return nullptr;
        }

        auto impl = std::make_unique<Impl>(devices.front(), *description, softening);
        return std::unique_ptr<SyclDirectSolver>{new SyclDirectSolver{std::move(impl)}};
    } catch (const sycl::exception&) {
        // Queue creation is where a driver that enumerated a device but cannot
        // actually use it fails. Reported as no device, for the reason
        // `backend/sycl_device.hpp` gives: the caller's question is whether
        // there is a GPU to run on, and every way of answering no is the same
        // answer.
        return nullptr;
    }
}

SyclDirectSolver::SyclDirectSolver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

SyclDirectSolver::~SyclDirectSolver() = default;
SyclDirectSolver::SyclDirectSolver(SyclDirectSolver&&) noexcept = default;
SyclDirectSolver& SyclDirectSolver::operator=(SyclDirectSolver&&) noexcept = default;

core::Softening SyclDirectSolver::softening() const noexcept { return impl_->softening; }

InteractionCount SyclDirectSolver::interaction_count() const noexcept { return impl_->count; }

void SyclDirectSolver::reset_interaction_count() noexcept { impl_->count = {}; }

const backend::DeviceDescription& SyclDirectSolver::device() const noexcept {
    return impl_->description;
}

Index SyclDirectSolver::tile_size() const noexcept { return impl_->tile; }

const SyclEvaluationTimings& SyclDirectSolver::timings() const noexcept { return impl_->timings; }

bool SyclDirectSolver::uses_shared_memory() const noexcept {
    if (impl_->position_x.empty()) {
        return false;
    }
    return backend::allocation_kind(impl_->position_x.data(), impl_->queue) ==
           backend::UsmKind::kShared;
}

void SyclDirectSolver::evaluate(Vec3Span<const Real> positions, std::span<const Real> masses,
                                Vec3Span<Real> accelerations) {
    const Index count = positions.size();
    if (count == 0) {
        ++impl_->count.evaluations;
        impl_->timings = {};
        return;
    }

    impl_->ensure_capacity(count);

    const auto staging_in_began = backend::Clock::now();

    // Staged into the solver's own arrays rather than read from the caller's.
    //
    // This is a copy, and it is worth being exact about which one. It is host
    // memory to host memory: both the source and the destination are in the
    // 32 GB the CPU and GPU share, and nothing crosses a bus, because on this
    // part there is no bus to cross. What Phase 9 requires the absence of is a
    // host-to-device transfer, and there is none: the pointer written here is
    // the pointer the kernel dereferences, which `uses_shared_memory` asks the
    // runtime to confirm.
    //
    // It costs O(N) against the kernel's O(N^2) and is measured rather than
    // waved away, in `docs/performance/sycl_direct.md`. ADR-0027 records why the
    // particle arrays are not simply allocated in USM to begin with, which would
    // remove it.
    std::copy_n(positions.x.data(), count, impl_->position_x.data());
    std::copy_n(positions.y.data(), count, impl_->position_y.data());
    std::copy_n(positions.z.data(), count, impl_->position_z.data());
    std::copy_n(masses.data(), count, impl_->mass.data());

    // The padded tail carries zero mass, so a tail source contributes nothing to
    // any sum. That is what lets the inner loop run over whole tiles without a
    // bound check on the source index. Positions there are left as they were,
    // which is harmless precisely because the mass is zero.
    const Index padded = ((count + impl_->tile - 1) / impl_->tile) * impl_->tile;
    std::fill(impl_->mass.data() + count, impl_->mass.data() + padded, Real{0});
    std::fill(impl_->position_x.data() + count, impl_->position_x.data() + padded, Real{0});
    std::fill(impl_->position_y.data() + count, impl_->position_y.data() + padded, Real{0});
    std::fill(impl_->position_z.data() + count, impl_->position_z.data() + padded, Real{0});

    const auto kernel_began = backend::Clock::now();
    impl_->timings.staging_in = kernel_began - staging_in_began;

    const Index tile = impl_->tile;
    const core::Softening softening = impl_->softening;

    // Raw pointers into the kernel, which is the one place section 4 of the
    // implementation plan permits them over spans, and this is that place: a
    // captured `std::span` would carry a length the kernel never reads, and the
    // capture has to be trivially copyable to cross to the device at all.
    const Real* const position_x = impl_->position_x.data();
    const Real* const position_y = impl_->position_y.data();
    const Real* const position_z = impl_->position_z.data();
    const Real* const mass = impl_->mass.data();
    Real* const acceleration_x = impl_->acceleration_x.data();
    Real* const acceleration_y = impl_->acceleration_y.data();
    Real* const acceleration_z = impl_->acceleration_z.data();

    impl_->queue.submit([&](sycl::handler& handler) {
        sycl::local_accessor<Real, 1> tile_x{sycl::range<1>{tile}, handler};
        sycl::local_accessor<Real, 1> tile_y{sycl::range<1>{tile}, handler};
        sycl::local_accessor<Real, 1> tile_z{sycl::range<1>{tile}, handler};
        sycl::local_accessor<Real, 1> tile_mass{sycl::range<1>{tile}, handler};

        handler.parallel_for(
            sycl::nd_range<1>{sycl::range<1>{padded}, sycl::range<1>{tile}},
            [=](sycl::nd_item<1> item) {
                const Index target = item.get_global_id(0);
                const Index lane = item.get_local_id(0);

                const Real x = position_x[target];
                const Real y = position_y[target];
                const Real z = position_z[target];

                Real sum_x{0};
                Real sum_y{0};
                Real sum_z{0};

                for (Index base = 0; base < padded; base += tile) {
                    // One cooperative load per work-item, then the whole group
                    // reads the tile out of local memory. This is the reuse the
                    // kernel exists for: each source position is fetched from
                    // global memory once per group rather than once per target.
                    const Index source = base + lane;
                    tile_x[lane] = position_x[source];
                    tile_y[lane] = position_y[source];
                    tile_z[lane] = position_z[source];
                    tile_mass[lane] = mass[source];

                    sycl::group_barrier(item.get_group());

                    for (Index j = 0; j < tile; ++j) {
                        const Real dx = tile_x[j] - x;
                        const Real dy = tile_y[j] - y;
                        const Real dz = tile_z[j] - z;

                        // The self term, masked in both factors. Selecting the
                        // mass to zero alone would leave a division by zero in
                        // an unsoftened run, and `inf * 0` is NaN rather than
                        // the zero contribution intended. Selecting the squared
                        // separation to one keeps the reciprocal finite so that
                        // the zero mass can do its job.
                        const bool self = (base + j) == target;
                        const Real separation_squared =
                            self ? Real{1} : (dx * dx + dy * dy + dz * dz);
                        const Real source_mass = self ? Real{0} : tile_mass[j];

                        // The same function the CPU kernel and the potential
                        // energy diagnostic call, compiled for the device. That
                        // is the property single-source buys and the reason
                        // ADR-0025 weighs it above the alternatives: there is
                        // one definition of the softened force in this project,
                        // not one per backend.
                        const Real factor =
                            source_mass *
                            core::softened_inverse_distance_cubed(separation_squared, softening);

                        sum_x += dx * factor;
                        sum_y += dy * factor;
                        sum_z += dz * factor;
                    }

                    // Before overwriting the tile on the next pass. Without this
                    // a fast work-item would load the next tile over values a
                    // slow one in the same group is still reading.
                    sycl::group_barrier(item.get_group());
                }

                // G is one in this project's units (ADR-0007), written for the
                // reason `direct_solver.cpp` gives rather than for its value.
                acceleration_x[target] = core::kGravitationalConstant * sum_x;
                acceleration_y[target] = core::kGravitationalConstant * sum_y;
                acceleration_z[target] = core::kGravitationalConstant * sum_z;
            });
    });

    // The kernel writes shared memory the host is about to read, so the wait is
    // not merely for timing. Reading a USM allocation while a kernel that writes
    // it is in flight is a data race whatever the memory is shared with.
    impl_->queue.wait_and_throw();

    const auto staging_out_began = backend::Clock::now();
    impl_->timings.kernel = staging_out_began - kernel_began;

    std::copy_n(impl_->acceleration_x.data(), count, accelerations.x.data());
    std::copy_n(impl_->acceleration_y.data(), count, accelerations.y.data());
    std::copy_n(impl_->acceleration_z.data(), count, accelerations.z.data());

    impl_->timings.staging_out = backend::Clock::now() - staging_out_began;

    ++impl_->count.evaluations;

    // N(N-1), the same closed form the CPU solver reports, so that the two are
    // comparable. The kernel evaluates N padded to a multiple of the tile,
    // squared, and masks the difference: the padded sources carry zero mass and
    // the self term is selected away. Counting what the hardware issued rather
    // than what the physics required would make a GPU row incomparable with
    // every other row in the same table, and the padding is a property of the
    // tile size rather than of the algorithm.
    if (count > 1) {
        const auto pairs = static_cast<std::uint64_t>(count);
        impl_->count.particle_particle += pairs * (pairs - 1);
    }
}

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_SYCL
