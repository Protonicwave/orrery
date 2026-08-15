#pragma once

/// \file
/// This device's own ceilings, as three kernels that do nothing else.
///
/// Phase 7 established the rule the project has followed since: a performance
/// figure is quoted as a fraction of a limit measured on the machine in front of
/// you, not of a manufacturer's peak. Phase 9 measured the same three limits on
/// the integrated GPU rather than assuming the CPU's answers carried over, and
/// this is the third machine to be asked.
///
/// It matters more here than it has before, because this is the first device in
/// the project that nobody working on it owns. The figures in
/// `docs/performance/cuda.md` come from a hosted notebook, and a hosted notebook
/// is a shared machine of unknown provenance: the card may be throttled, it may
/// be sharing a host with other work, and the specification sheet for the part
/// is the least reliable guide available to what it will actually do. Probes
/// that run in the same session as the tables are the only figures that describe
/// the machine the tables came from.
///
/// The kernels are the CUDA spellings of the three in `benchmarks/sycl_direct.cpp`,
/// with the same accounting, so that the two documents' ceiling tables can be put
/// beside each other. Each keeps several independent accumulators in registers,
/// because a single chain would measure the latency of an operation rather than
/// the throughput of the pipelines, and each writes its total to memory at the
/// end so that nothing can be discarded as dead.
///
/// Timing does not happen here. These functions launch and synchronise; the
/// harness in `benchmarks/harness/protocol.hpp` runs them under the protocol,
/// which is the same discipline `solvers/cuda_kernels.hpp` applies to the
/// solvers and for the same reason: only the kernel crosses to the device
/// compiler.

#ifdef ORRERY_ENABLE_CUDA

#    include <cstddef>

#    include <cuda_runtime.h>

namespace orrery::benchmark {

/// How many independent accumulators each arithmetic probe keeps.
///
/// Exposed because the operation count the caller computes has to match what the
/// kernel performed, and a constant repeated in two files is a constant that will
/// eventually differ between them.
inline constexpr unsigned kFmaAccumulators = 8;
inline constexpr unsigned kDivSqrtAccumulators = 4;

/// Fused multiply-add throughput, with no memory in the loop.
///
/// `output` must hold `items` floats. Two floating-point operations per fused
/// multiply-add, which is the accounting the CPU probe in
/// `harness/arithmetic_probe.hpp` uses, so the three ceilings are in the same
/// units.
[[nodiscard]] cudaError_t launch_fma_probe(float* output, unsigned items, unsigned iterations);

/// Square root and divide throughput, the ceiling the force kernel competes for
/// on the CPU and did not on the integrated GPU.
///
/// One square root and one division per step, counted as two operations, which
/// is how `harness/machine_limits.hpp` counts them. The square root feeds its own
/// accumulator so the compiler cannot hoist it out of the loop.
[[nodiscard]] cudaError_t launch_div_sqrt_probe(float* output, unsigned items, unsigned iterations);

/// Read bandwidth, from a working set far larger than any cache on the part.
///
/// A sum rather than a copy, because reading is what the force kernel does to the
/// source arrays and a triad would measure a mixture it never performs. `sums`
/// must hold one float per block. Consecutive threads read consecutive elements,
/// so each warp's reads coalesce into whole cache lines; a blocked division would
/// have every lane on a different line and would measure the wrong thing
/// entirely.
///
/// This is the probe whose result Phase 9 had to qualify rather than defend: on
/// the integrated part it measured well under what the CPU sustains on the same
/// physical memory, which was more likely a limitation of the probe than of the
/// device. On a discrete card with its own memory the same probe has a much
/// better chance of being meaningful, and it is worth knowing whether it is.
[[nodiscard]] cudaError_t launch_read_probe(const float* input, float* sums, std::size_t elements,
                                            unsigned blocks, unsigned block);

} // namespace orrery::benchmark

#endif // ORRERY_ENABLE_CUDA
