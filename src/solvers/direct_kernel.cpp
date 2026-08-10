#include "orrery/solvers/direct_kernel.hpp"

#include <string_view>

#include "orrery/backend/cpu_features.hpp"
#include "orrery/core/types.hpp"

namespace orrery::solvers {

bool kernel_available(KernelKind kind) noexcept {
    switch (kind) {
    case KernelKind::kScalar:
        // Always, on every machine and in every build. A reference that could
        // be absent would not be a reference.
        return true;

    case KernelKind::kAvx2:
#ifdef ORRERY_HAS_AVX2_KERNEL
        // Both halves of the question. The kernel exists in this binary because
        // the build compiled it, and the processor can execute it because it
        // reports the two feature bits and the operating system has enabled the
        // vector state that comes with them. FMA is asked for separately from
        // AVX2 because the kernel accumulates with it, and a processor with one
        // and not the other would fault on the instruction this project
        // actually issues rather than on the one it checked for.
        return backend::cpu_features().avx2 && backend::cpu_features().fma;
#else
        return false;
#endif
    }

    return false;
}

KernelKind fastest_available_kernel() noexcept {
    // Ordered fastest first, and the ordering is the claim: AVX2 is faster than
    // scalar on every machine that has it, which `docs/performance/roofline.md`
    // measures rather than assumes. A later phase adding a third kernel adds it
    // at the position its measurement earns.
    if (kernel_available(KernelKind::kAvx2)) {
        return KernelKind::kAvx2;
    }

    return KernelKind::kScalar;
}

AccumulateRange accumulate_range_for(KernelKind kind) noexcept {
#ifdef ORRERY_HAS_AVX2_KERNEL
    if (kind == KernelKind::kAvx2 && kernel_available(kind)) {
        return &accumulate_range_avx2;
    }
#else
    static_cast<void>(kind);
#endif

    // The fallback, and the reason this function cannot return null. A caller
    // that asked for a kernel this machine does not have gets a correct answer
    // rather than a crash, and finds out which kernel it got by asking
    // `DirectSolver::kernel()` or `kernel_available` rather than by comparing
    // the timings against what it expected.
    return &accumulate_range_scalar;
}

core::Index kernel_lane_count(KernelKind kind) noexcept {
    switch (kind) {
    case KernelKind::kScalar:
        return 1;
    case KernelKind::kAvx2:
        // 256 bits, whatever this build calls a scalar.
        return 32 / sizeof(core::Real);
    }

    return 1;
}

std::string_view to_string(KernelKind kind) noexcept {
    switch (kind) {
    case KernelKind::kScalar:
        return "scalar";
    case KernelKind::kAvx2:
        return "avx2";
    }

    return "unknown";
}

} // namespace orrery::solvers
