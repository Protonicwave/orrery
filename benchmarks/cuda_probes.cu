#include "cuda_probes.hpp"

#ifdef ORRERY_ENABLE_CUDA

#    include <cstddef>

#    include <cuda_runtime.h>

namespace orrery::benchmark {

namespace {

constexpr unsigned kProbeBlock = 256;

__global__ void fma_probe(float* output, unsigned items, unsigned iterations) {
    const unsigned id = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (id >= items) {
        return;
    }

    float accumulator[kFmaAccumulators];
    for (unsigned a = 0; a < kFmaAccumulators; ++a) {
        accumulator[a] = static_cast<float>(a) + 1.0F;
    }

    // A multiplier that varies between threads and an addend that does not, so
    // that the compiler cannot fold the whole loop into a closed form while the
    // arithmetic stays a fused multiply-add per accumulator per step.
    const float multiplier = static_cast<float>(id & 3U) + 1.0F;
    constexpr float kAddend = 1.000001F;

    for (unsigned i = 0; i < iterations; ++i) {
        for (unsigned a = 0; a < kFmaAccumulators; ++a) {
            accumulator[a] = (accumulator[a] * kAddend) + multiplier;
        }
    }

    float total = 0;
    for (unsigned a = 0; a < kFmaAccumulators; ++a) {
        total += accumulator[a];
    }
    output[id] = total;
}

__global__ void div_sqrt_probe(float* output, unsigned items, unsigned iterations) {
    const unsigned id = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (id >= items) {
        return;
    }

    float accumulator[kDivSqrtAccumulators];
    for (unsigned a = 0; a < kDivSqrtAccumulators; ++a) {
        accumulator[a] = static_cast<float>(id + a) + 1.0F;
    }

    for (unsigned i = 0; i < iterations; ++i) {
        for (unsigned a = 0; a < kDivSqrtAccumulators; ++a) {
            // The same shape the force kernel uses: a reciprocal of a square
            // root of something that changes every step. Written as a division
            // by a square root rather than as an intrinsic, because the intrinsic
            // is the approximate instruction and the force kernel does not use
            // it: ADR-0020 declines fast-math and the probe has to measure the
            // ceiling the kernel is actually competing for.
            accumulator[a] = 1.0F + (1.0F / sqrtf(accumulator[a] + 1.0F));
        }
    }

    float total = 0;
    for (unsigned a = 0; a < kDivSqrtAccumulators; ++a) {
        total += accumulator[a];
    }
    output[id] = total;
}

__global__ void read_probe(const float* input, float* sums, std::size_t elements) {
    const std::size_t stride = static_cast<std::size_t>(gridDim.x) * blockDim.x;

    float total = 0;
    for (std::size_t i = (blockIdx.x * blockDim.x) + threadIdx.x; i < elements; i += stride) {
        total += input[i];
    }

    sums[(blockIdx.x * blockDim.x) + threadIdx.x] = total;
}

/// Launch, check that the driver accepted it, then wait.
///
/// The same two-stage check the solver launches make, for the same reason: a
/// configuration the driver refused would otherwise be reported by the
/// synchronisation and read as a fault during execution.
[[nodiscard]] cudaError_t finish() {
    const cudaError_t launched = cudaGetLastError();
    if (launched != cudaSuccess) {
        return launched;
    }
    return cudaDeviceSynchronize();
}

} // namespace

cudaError_t launch_fma_probe(float* output, unsigned items, unsigned iterations) {
    const unsigned blocks = (items + kProbeBlock - 1) / kProbeBlock;
    fma_probe<<<blocks, kProbeBlock>>>(output, items, iterations);
    return finish();
}

cudaError_t launch_div_sqrt_probe(float* output, unsigned items, unsigned iterations) {
    const unsigned blocks = (items + kProbeBlock - 1) / kProbeBlock;
    div_sqrt_probe<<<blocks, kProbeBlock>>>(output, items, iterations);
    return finish();
}

cudaError_t launch_read_probe(const float* input, float* sums, std::size_t elements,
                              unsigned blocks, unsigned block) {
    read_probe<<<blocks, block>>>(input, sums, elements);
    return finish();
}

} // namespace orrery::benchmark

#endif // ORRERY_ENABLE_CUDA
