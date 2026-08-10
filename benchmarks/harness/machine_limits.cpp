#include "harness/machine_limits.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "harness/arithmetic_probe.hpp"
#include "harness/protocol.hpp"
#include "harness/statistics.hpp"
#include "orrery/backend/cpu_features.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"

namespace orrery::benchmark {

namespace {

using core::Index;
using core::Real;

/// The same storage the particle arrays use, for the same reason.
///
/// A bandwidth probe run over storage aligned differently from the storage the
/// kernels use would measure a different path through the cache. See
/// `core/aligned_allocator.hpp`.
using Buffer = std::vector<Real, core::AlignedAllocator<Real>>;

/// How many independent accumulators the read probe keeps.
///
/// One would make the probe a chain of dependent additions four cycles apart,
/// which reaches nothing like the memory bandwidth and would report the latency
/// of floating-point addition under a bandwidth heading. Eight is enough to
/// leave the loads as the limit, which is what is being measured.
constexpr Index kReadAccumulators = 8;

[[nodiscard]] Real sum_range(const Real* data, Index begin, Index end) noexcept {
    std::array<Real, kReadAccumulators> accumulators{};

    Index index = begin;
    for (; index + kReadAccumulators <= end; index += kReadAccumulators) {
        for (Index lane = 0; lane < kReadAccumulators; ++lane) {
            accumulators[lane] += data[index + lane];
        }
    }

    Real total = 0;
    for (const Real accumulator : accumulators) {
        total += accumulator;
    }

    for (; index < end; ++index) {
        total += data[index];
    }

    return total;
}

void triad_range(Real* result, const Real* left, const Real* right, Real scale, Index begin,
                 Index end) noexcept {
    for (Index index = begin; index < end; ++index) {
        result[index] = left[index] + (scale * right[index]);
    }
}

/// How many elements of `Real` fit in a byte budget.
[[nodiscard]] Index elements_in(std::size_t bytes) {
    const Index elements = bytes / sizeof(Real);

    // A probe over nothing would divide by a zero duration and report an
    // infinite bandwidth. One cache line is the smallest request that means
    // anything at all.
    return elements == 0 ? core::kCacheLineBytes / sizeof(Real) : elements;
}

/// Somewhere for a result to go that the optimiser cannot prove is unread.
///
/// Every probe here computes something nobody wants, and a loop whose output is
/// discarded is deleted at the optimisation level the project measures at. An
/// atomic rather than a `volatile` because the chunks run on several threads at
/// once, and because relaxed ordering costs one uncontended cache line per
/// chunk rather than anything per element.
using Sink = std::atomic<Real>;

void deposit(Sink& sink, Real value) noexcept {
    sink.fetch_add(value, std::memory_order_relaxed);
}

/// Whether the vector probes may be executed here.
[[nodiscard]] bool use_vector_probes() noexcept {
#ifdef ORRERY_HAS_AVX2_PROBE
    // FMA asked for beside AVX2 because both probes issue instructions from
    // both feature sets, and a part with one and not the other would fault on
    // exactly the instruction that was not checked for.
    return backend::cpu_features().avx2 && backend::cpu_features().fma;
#else
    return false;
#endif
}

/// One block of arithmetic: performs the work and returns how much of it there
/// was.
using ArithmeticProbe = double (*)(std::uint64_t rounds, Real* sink) noexcept;

[[nodiscard]] ArithmeticProbe multiply_add_probe() noexcept {
#ifdef ORRERY_HAS_AVX2_PROBE
    if (use_vector_probes()) {
        return &fused_multiply_add_block_avx2;
    }
#endif
    return &fused_multiply_add_block_scalar;
}

[[nodiscard]] ArithmeticProbe divide_and_sqrt_probe() noexcept {
#ifdef ORRERY_HAS_AVX2_PROBE
    if (use_vector_probes()) {
        return &divide_and_sqrt_block_avx2;
    }
#endif
    return &divide_and_sqrt_block_scalar;
}

/// Run an arithmetic probe on every worker and time it.
///
/// Blocks rather than one piece of work per worker. A scheme that gave each
/// worker exactly one index would be at the mercy of how the executor chunks,
/// and on this machine's asymmetric cores an equal split would measure four
/// performance cores waiting for four efficiency ones rather than the machine.
/// Sixteen blocks per worker is enough for the work-stealing executor to
/// balance them, by the argument ADR-0016 makes for the force loop.
[[nodiscard]] Limit measure_arithmetic(backend::Executor& executor, const Protocol& protocol,
                                       std::string name, std::string unit, ArithmeticProbe probe) {
    constexpr Index kBlocksPerWorker = 16;

    // Chosen so that a trial is a few tens of milliseconds on the target part:
    // long enough to be far above the clock's resolution, short enough that
    // eleven trials plus a warm-up do not themselves heat the machine into a
    // different regime.
    constexpr std::uint64_t kRoundsPerBlock = 200000;

    const Index blocks = Index{executor.worker_count()} * kBlocksPerWorker;

    Sink sink{};
    std::atomic<double> operations{0};

    const TrialSet trials = run_trials(protocol, [&] {
        operations.store(0, std::memory_order_relaxed);

        executor.run(blocks, [&](Index begin, Index end) {
            Real local = 0;
            double performed = 0;

            for (Index block = begin; block < end; ++block) {
                performed += probe(kRoundsPerBlock, &local);
            }

            deposit(sink, local);
            operations.fetch_add(performed, std::memory_order_relaxed);
        });
    });

    return Limit{.name = std::move(name),
                 .unit = std::move(unit),
                 .quantity_per_trial = operations.load(std::memory_order_relaxed),
                 .trials = trials};
}

} // namespace

Limit measure_read_bandwidth(backend::Executor& executor, const Protocol& protocol,
                             std::size_t bytes) {
    const Index elements = elements_in(bytes);

    // Filled at construction, which touches every page. The warm-up trials then
    // have nothing left to fault in and the timed trials measure the path from
    // memory rather than the path through the page fault handler.
    Buffer buffer(elements, static_cast<Real>(1));
    Sink sink{};

    const TrialSet trials = run_trials(protocol, [&] {
        executor.run(elements, [&](Index begin, Index end) {
            deposit(sink, sum_range(buffer.data(), begin, end));
        });
    });

    return Limit{.name = "read",
                 .unit = "bytes",
                 .quantity_per_trial = static_cast<double>(elements) * sizeof(Real),
                 .trials = trials};
}

Limit measure_triad_bandwidth(backend::Executor& executor, const Protocol& protocol,
                              std::size_t bytes) {
    // Three buffers sharing the byte budget, so the footprint matches the read
    // probe's. Comparing a triad over 768 MiB against a read over 256 MiB would
    // confound the two shapes with how much of each fitted where.
    const Index elements = elements_in(bytes / 3);

    Buffer result(elements, static_cast<Real>(0));
    Buffer left(elements, static_cast<Real>(1));
    Buffer right(elements, static_cast<Real>(2));

    const auto scale = static_cast<Real>(3);

    const TrialSet trials = run_trials(protocol, [&] {
        executor.run(elements, [&](Index begin, Index end) {
            triad_range(result.data(), left.data(), right.data(), scale, begin, end);
        });
    });

    Sink sink{};
    deposit(sink, result[elements / 2]);

    // Two read and one written per element, which is the STREAM convention. The
    // header says why that understates the traffic on a write-allocate machine
    // and what the other convention would give.
    return Limit{.name = "triad",
                 .unit = "bytes",
                 .quantity_per_trial = 3.0 * static_cast<double>(elements) * sizeof(Real),
                 .trials = trials};
}

Limit measure_peak_throughput(backend::Executor& executor, const Protocol& protocol) {
    return measure_arithmetic(executor, protocol, "fused multiply-add", "flop",
                              multiply_add_probe());
}

Limit measure_divide_and_sqrt_throughput(backend::Executor& executor, const Protocol& protocol) {
    return measure_arithmetic(executor, protocol, "divide and square root", "op",
                              divide_and_sqrt_probe());
}

double fused_multiply_add_block(std::uint64_t rounds, Real* sink) noexcept {
    return multiply_add_probe()(rounds, sink);
}

std::string_view throughput_probe_name() noexcept {
    return use_vector_probes() ? "avx2" : "scalar";
}

} // namespace orrery::benchmark
