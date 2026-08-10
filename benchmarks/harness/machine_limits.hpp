#pragma once

/// \file
/// The two ceilings every kernel in this project is judged against, measured on
/// the machine in front of us rather than read off a specification.
///
/// Section 2 of the implementation plan records the manufacturer's figures for
/// the target part: roughly 135 GB/s of memory bandwidth shared between the CPU
/// and the integrated GPU, and a peak floating-point rate implied by the core
/// count, the vector width and the clock. It also says that those are nominal
/// and that Phase 7 measures them directly, after which the measured values are
/// what the project quotes. This file is that measurement.
///
/// ## Why it has to be measured
///
/// A nominal bandwidth is the product of a bus width and a clock. No program
/// reaches it, and how close a program can get is a property of the memory
/// controller, the prefetchers, the number of outstanding requests a core can
/// have and how many cores are asking, none of which appear in the
/// multiplication. A roofline drawn against the nominal figure would place
/// every kernel in this project further below the line than it is, and the gap
/// would be attributed to the kernel rather than to the machine.
///
/// The nominal arithmetic peak is worse, because it is not one number. It
/// depends on which instruction, at what width, in what precision, and on
/// whether the part sustains its boost clock for the duration. On this
/// processor it also depends on which of the two kinds of core is running,
/// which is exactly the asymmetry Phase 6 measured.
///
/// ## What each probe is
///
/// The bandwidth probes are the two shapes that matter for this project. A
/// read-only sum, which is what a force kernel streaming positions and masses
/// does, and a triad `a = b + s * c`, which is what an integrator updating
/// positions from velocities does. Both run over buffers many times the size of
/// the 8 MB last-level cache, so that what is measured is the path to memory
/// and not the path to L3.
///
/// The arithmetic probes are two, and `arithmetic_probe.hpp` explains why. One
/// is a chain of fused multiply-adds, which is the operation every published
/// peak figure is quoted in. The other is a chain of square roots and
/// divisions, which is what the direct kernel actually spends its time on and
/// what therefore decides how fast a kernel of this shape can possibly be. Both
/// keep enough independent accumulators that no instruction waits for the one
/// before it, and both touch no memory at all.
///
/// The multiply-add probe doubles as the thermal canary in `protocol.hpp`,
/// since a workload that cannot be slowed by anything except the clock is
/// exactly what is wanted for measuring the clock.
///
/// ## What these numbers are not
///
/// They are the ceilings this machine reached under this project's own probes,
/// on the day and at the temperature recorded beside them. They are not
/// hardware constants. A part that has been at full load for ten minutes has a
/// lower ceiling than one that has been idle, which is why every figure is
/// reported with its dispersion, its drift and the canary's verdict on the
/// session that produced it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "harness/protocol.hpp"
#include "harness/statistics.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/core/types.hpp"

namespace orrery::benchmark {

/// One measured hardware ceiling.
struct Limit {
    /// What was probed, for the table.
    std::string name;

    /// What the rate counts: bytes for the bandwidth probes, floating-point
    /// operations for the arithmetic one.
    std::string unit;

    /// How much of `unit` one trial moved or performed.
    ///
    /// Kept beside the timings rather than folded into a rate, so that a
    /// reader can check the rate against the work and the duration
    /// independently. A bandwidth figure whose byte count nobody can see is a
    /// number with a convention hidden inside it.
    double quantity_per_trial{};

    /// The timings, with everything `TrialSet` reports about them.
    TrialSet trials;

    /// The sustained rate, from the median trial. This is the figure to quote.
    [[nodiscard]] double rate() const noexcept {
        return rate_per_second(quantity_per_trial, trials.median());
    }

    /// The rate from the fastest trial, which is the number a best-of
    /// convention would have reported.
    ///
    /// Present so that the difference between the two conventions is visible in
    /// this project's own output rather than only argued about in
    /// `statistics.hpp`.
    [[nodiscard]] double best_rate() const noexcept {
        return rate_per_second(quantity_per_trial, trials.fastest());
    }
};

/// How much memory the bandwidth probes stream, by default.
///
/// 512 MiB, which is sixty-four times the 8 MB last-level cache of the target
/// part. A buffer that fitted in cache would measure the cache, and one only a
/// few times larger would still be substantially served by it. The size was
/// raised from 256 MiB after the first session: a trial that took four
/// milliseconds reported an interquartile range of half its own median, because
/// anything else touching the memory system during those four milliseconds
/// moved it. Twice the buffer is twice the trial, and the dispersion falls with
/// it.
inline constexpr std::size_t kDefaultStreamBytes = std::size_t{512} << 20U;

/// Sustained read bandwidth: one pass over a large buffer, summing it.
///
/// Counts one byte per byte read, which for a read-only pass is unambiguous.
/// The sum is computed into several independent accumulators rather than one,
/// because a single running sum is a dependency chain of one addition per
/// element and would make this a probe of floating-point latency instead of
/// memory bandwidth.
[[nodiscard]] Limit measure_read_bandwidth(backend::Executor& executor, const Protocol& protocol,
                                           std::size_t bytes = kDefaultStreamBytes);

/// Sustained triad bandwidth: `a = b + s * c` over three large buffers.
///
/// Counts three bytes per element per scalar: two read and one written. That is
/// the STREAM convention and it understates the traffic on this and every other
/// write-allocate architecture, where storing to a line that is not in cache
/// first fetches it, making the true figure four. Both conventions are in
/// circulation; this one is stated here so that the number can be converted
/// rather than guessed at.
[[nodiscard]] Limit measure_triad_bandwidth(backend::Executor& executor, const Protocol& protocol,
                                            std::size_t bytes = kDefaultStreamBytes);

/// Sustained floating-point throughput, from dependency-free fused
/// multiply-adds.
///
/// Counts two operations per fused multiply-add, which is the universal
/// convention and the one every published peak figure uses. This is the flat
/// section of the roofline.
[[nodiscard]] Limit measure_peak_throughput(backend::Executor& executor, const Protocol& protocol);

/// Sustained square root and division throughput.
///
/// The ceiling that actually binds the direct kernel, and the reason this
/// project reports two arithmetic limits rather than one. Every pairwise
/// interaction contains one square root and one division, both of which retire
/// on a unit with a small fraction of the throughput of the multiply-add
/// pipelines, so a kernel of this shape cannot approach the peak above however
/// well it is written. `arithmetic_probe.hpp` sets out the argument and
/// `docs/performance/roofline.md` states what the two ceilings turned out to be
/// on this machine.
///
/// Counts one operation per square root and one per division, so the figure is
/// directly comparable with the two such operations the kernel performs per
/// interaction.
[[nodiscard]] Limit measure_divide_and_sqrt_throughput(backend::Executor& executor,
                                                       const Protocol& protocol);

/// The arithmetic probe on the calling thread, for the thermal canary.
///
/// Performs `rounds` blocks of independent fused multiply-adds and returns how
/// many floating-point operations that was. The arithmetic result is written to
/// `sink`, which the caller must not discard: without somewhere for the answer
/// to go, the whole loop is dead code and an optimiser will delete it.
double fused_multiply_add_block(std::uint64_t rounds, core::Real* sink) noexcept;

/// How many blocks the canary runs.
///
/// Chosen so that one call takes a few milliseconds on the target part: long
/// enough that the clock has settled within the call and that the timing is far
/// above the clock's resolution, short enough that marking the canary between
/// configurations does not itself heat the machine it is measuring.
inline constexpr std::uint64_t kCanaryRounds = 200000;

/// Which arithmetic probe the machine is running, for the report.
[[nodiscard]] std::string_view throughput_probe_name() noexcept;

} // namespace orrery::benchmark
