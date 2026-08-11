#pragma once

/// \file
/// Direct summation on the integrated GPU.
///
/// The same physics as `solvers/direct_solver.hpp`, over the same softened
/// potential from `core/softening.hpp`, in the same units. What differs is where
/// the loop runs and how the sources are staged for it. This is a backend behind
/// the existing solver interface rather than a second solver, which is the
/// distinction section 3 of the implementation plan draws and ADR-0026 records
/// the seam for.
///
/// ## The kernel
///
/// One work-item per target particle. Each accumulates the acceleration on its
/// own particle from every source, so the write pattern is the same one ADR-0015
/// chose in Phase 5: read everything, write only yourself, no reduction between
/// work-items and no atomics anywhere.
///
/// Sources are staged through shared local memory a tile at a time. Without that
/// every work-item in a group reads the same source position from global memory
/// on the same instruction, which the hardware coalesces but still fetches
/// repeatedly across the N/tile passes. With it, a group cooperatively loads one
/// tile, synchronises, and then reads it from memory local to the Xe-core. That
/// turns the kernel's global memory traffic from O(N^2) into O(N^2 / tile),
/// which matters because section 2 of the plan establishes bandwidth as the
/// binding constraint on this part rather than arithmetic.
///
/// ## Self-interaction
///
/// The CPU kernel excludes a particle from its own sum by splitting the range
/// either side of its index, which costs nothing in a loop. That trick does not
/// survive tiling: a tile is a block of sources shared by a whole work-group,
/// and each work-item has a different index to exclude. So the self term is
/// masked instead. Both the mass and the squared separation are selected, the
/// first to zero and the second to one, because zeroing the mass alone would
/// leave an infinite reciprocal distance in an unsoftened run and produce a NaN
/// from `inf * 0` rather than the zero contribution intended.
///
/// ## What it will and will not agree with
///
/// Not bit-for-bit with the CPU solver, for the reason the AVX2 kernel is not:
/// the sum is reassociated. Here it is reassociated further, since the order is
/// tile by tile rather than index by index, and the device is entitled to
/// contract a multiply and an add into an FMA. Neither is a relaxation of IEEE
/// arithmetic and ADR-0020's rule against fast-math flags is not weakened.
///
/// The larger effect is precision, and not in the way integrated GPUs are
/// usually assumed to behave. The target device reports the `fp64` aspect, so a
/// double-precision build constructs and runs rather than being declined. What
/// the aspect does not promise is a useful rate, and the gap between reporting
/// double precision and executing it quickly is exactly the sort of claim this
/// project measures rather than assumes:
/// `docs/performance/sycl_direct.md` reports both configurations and the ratio
/// between them, which is what decides the precision a run should use.
///
/// Accuracy is quoted against the double-precision CPU direct solver in either
/// case, since that is the project's reference.
/// `tests/solvers/sycl_direct_solver_test.cpp` states the tolerance and derives
/// it from the precision the build was configured with.

#include <memory>
#include <span>
#include <string_view>

#include "orrery/backend/sycl_device.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/softening.hpp"
#include "orrery/core/types.hpp"
#include "orrery/core/vec3_span.hpp"
#include "orrery/solvers/force_solver.hpp"
#include "orrery/solvers/interaction_count.hpp"

#ifdef ORRERY_ENABLE_SYCL

namespace orrery::solvers {

/// Where the last evaluation spent its time.
///
/// ADR-0027 argues that the staging step is O(N) against the kernel's O(N^2) and
/// therefore stops mattering at the sizes the GPU is worth using at. That is an
/// argument, and this is what turns it into a measurement. A reader who suspects
/// the GPU figures are really measuring memcpy can check.
///
/// The same shape as `EvaluationTimings` in `solvers/barnes_hut_solver.hpp` and
/// for the same reason: a phase that adds a step to a force evaluation should
/// report what the step cost rather than leave it inside a total.
struct SyclEvaluationTimings {
    /// Copying positions and masses into the shared allocations, and zeroing the
    /// padded tail.
    backend::Duration staging_in{};

    /// Submitting the kernel and waiting for the device. This is the figure a
    /// GPU throughput number should be computed from.
    backend::Duration kernel{};

    /// Copying the accelerations back out.
    backend::Duration staging_out{};
};

/// Direct summation evaluated on a SYCL device.
///
/// Declared only where the backend is compiled, following the precedent
/// `ORRERY_HAS_AVX2_KERNEL` sets in `solvers/direct_kernel.hpp`. A caller that
/// must work either way asks `backend::kSyclBackendCompiled` first, which is a
/// constant and available in every build.
class SyclDirectSolver final : public ForceSolver {
public:
    /// A solver on this machine's GPU, or nothing.
    ///
    /// Returns null when there is no GPU, when the device cannot run the
    /// precision this build was configured for, or when the runtime fails to
    /// create a queue. All three mean the same thing to a caller, which is that
    /// this machine cannot run the kernel and the CPU solver should be used
    /// instead, and none of them is an error worth an exception: a laptop with
    /// no usable device is an ordinary machine, not a broken one.
    ///
    /// A factory rather than a constructor precisely because construction can
    /// fail for reasons that are not faults. A constructor has only one way to
    /// report failure and section 4 reserves that for setup errors.
    [[nodiscard]] static std::unique_ptr<SyclDirectSolver>
    try_create(core::Softening softening = {});

    ~SyclDirectSolver() override;

    SyclDirectSolver(const SyclDirectSolver&) = delete;
    SyclDirectSolver& operator=(const SyclDirectSolver&) = delete;
    SyclDirectSolver(SyclDirectSolver&&) noexcept;
    SyclDirectSolver& operator=(SyclDirectSolver&&) noexcept;

    /// Write the acceleration at each position into `accelerations`.
    ///
    /// The three views must describe the same particles in the same order and
    /// have the same length, as `AccelerationField` requires and as the CPU
    /// solver documents. Safe to call with no particles.
    ///
    /// Blocks until the device has finished. An asynchronous interface would be
    /// the right shape for overlapping the force evaluation with something else,
    /// and there is nothing else: the integrator cannot advance until the
    /// accelerations exist.
    void evaluate(core::Vec3Span<const core::Real> positions, std::span<const core::Real> masses,
                  core::Vec3Span<core::Real> accelerations) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "sycl-direct"; }

    [[nodiscard]] core::Softening softening() const noexcept override;

    [[nodiscard]] InteractionCount interaction_count() const noexcept override;

    void reset_interaction_count() noexcept override;

    /// What the runtime says about the device this solver runs on.
    ///
    /// Exposed because no GPU figure means anything without it. A throughput
    /// number from an integrated part and one from a discrete card are not
    /// comparable, and neither is the same part on a different driver.
    [[nodiscard]] const backend::DeviceDescription& device() const noexcept;

    /// How many sources are staged through local memory at a time, which is also
    /// the work-group size.
    ///
    /// Chosen from what the device reports rather than fixed, and reported so
    /// that a benchmark states it alongside the timing it produced.
    [[nodiscard]] core::Index tile_size() const noexcept;

    /// Where the last evaluation spent its time.
    [[nodiscard]] const SyclEvaluationTimings& timings() const noexcept;

    /// Whether the arrays the kernel reads are shared unified memory, asked of
    /// the runtime rather than assumed.
    ///
    /// This is the evidence behind Phase 9's zero-copy requirement, reachable
    /// from a test and from the benchmark that reports it.
    /// `backend/sycl_usm.hpp` explains what the question means and why the
    /// answer is not simply "I called the shared allocator".
    [[nodiscard]] bool uses_shared_memory() const noexcept;

private:
    /// SYCL's queue and USM handles are the private state, and putting them here
    /// would make `<sycl/sycl.hpp>` an include of every file that mentions a
    /// solver. The indirection costs one pointer hop per force evaluation,
    /// ahead of N^2 interactions on a device, which is the boundary section 3 of
    /// the implementation plan permits dispatch at and then some.
    struct Impl;

    explicit SyclDirectSolver(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace orrery::solvers

#endif // ORRERY_ENABLE_SYCL
