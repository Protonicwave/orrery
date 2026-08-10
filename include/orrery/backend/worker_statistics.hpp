#pragma once

/// \file
/// What each worker thread did, and how long it spent doing nothing.
///
/// Phase 6 exists to answer a question that a speedup figure cannot: when
/// eight threads run on four performance cores and four efficiency cores, how
/// much of the wall time do the fast cores spend waiting for the slow ones. A
/// scheduler that hides that behind a single number is a scheduler nobody can
/// improve, so the instrumentation is part of the design rather than something
/// bolted on for one benchmark.
///
/// The accounting is deliberately simple and closed. Within one parallel region
/// every worker is either running a chunk of the caller's work or it is not, and
/// the region lasts from the moment the submitting thread hands the work over to
/// the moment the last worker finishes. So for each worker
///
///     busy + idle = the duration of the region
///
/// and summing over W workers gives W times the region duration. Idle time
/// measured this way includes everything that is not the caller's arithmetic:
/// the wake-up latency of a sleeping thread, the wait for a chunk to become
/// available, and the wait at the end for slower workers to finish. That is
/// deliberate. Those costs are real, they are what a threading scheme is judged
/// on, and a definition that excluded them would flatter the scheduler by
/// measuring only the part of it that was already working.
///
/// The cost of the measurement is two clock readings per chunk. Chunks are sized
/// so that each is tens of microseconds of work at the sizes this project runs
/// at, against roughly a hundred nanoseconds for the pair of readings, so the
/// instrumentation is under a part in a hundred and is left permanently on. That
/// is worth more than the last fraction of a percent: a scheduler whose
/// behaviour can only be seen in a special build is a scheduler whose behaviour
/// is not seen.

#include <chrono>
#include <cstdint>
#include <vector>

#include "orrery/core/aligned_allocator.hpp"

namespace orrery::backend {

/// The clock every duration here is measured on.
///
/// `steady_clock` rather than `high_resolution_clock`, which on some standard
/// libraries is an alias for the system clock and therefore moves when the wall
/// clock is adjusted. An interval that can go backwards is not a measurement.
using Clock = std::chrono::steady_clock;

/// Durations are stored in nanoseconds so that they add without conversion and
/// mean the same thing on every platform, whatever the clock's native period is.
using Duration = std::chrono::nanoseconds;

/// One worker's record, accumulated over every region since the last reset.
struct WorkerStatistics {
    /// How many chunks of work this worker executed.
    std::uint64_t chunks{};

    /// How many items those chunks covered, an item being one index of the
    /// range the caller asked to have divided up. For the direct solver that is
    /// one particle, but the scheduler does not know or need to know that.
    ///
    /// This is the work-completed half of what Phase 6 reports. Under static
    /// partitioning it is the same for every worker by construction, which is
    /// the entire problem: the fast cores are given no more to do than the slow
    /// ones, so they finish early and wait.
    std::uint64_t items{};

    /// How many of those chunks were taken from another worker's range.
    ///
    /// Zero for every scheme that does not steal. For the work-stealing
    /// executor it is the direct evidence that dynamic balancing happened, and
    /// its distribution across workers should show the performance cores taking
    /// work from the efficiency cores rather than the reverse.
    std::uint64_t steals{};

    /// Time spent inside the caller's work.
    Duration busy{};

    /// Time inside a parallel region spent not working, as defined above.
    Duration idle{};
};

/// One worker's record, on a cache line of its own.
///
/// Workers update their own records while the others are updating theirs, and
/// two adjacent records in an array would share a cache line. Neither worker
/// would read the other's counters, but every increment would still take
/// exclusive ownership of the line away from the other core, which is precisely
/// the false sharing `partition.hpp` goes to some trouble to avoid in the
/// particle arrays. It would be a poor showing for the instrumentation that
/// measures a threading scheme to be the thing slowing it down.
struct alignas(core::kCacheLineBytes) PaddedWorkerStatistics {
    WorkerStatistics value;
};

/// The whole pool's record.
struct ExecutorStatistics {
    /// One entry per worker, in worker order.
    std::vector<WorkerStatistics> workers;

    /// Wall time spent inside parallel regions, measured by the submitting
    /// thread. This is the denominator a speedup is formed against.
    Duration elapsed{};

    /// How many parallel regions have run.
    std::uint64_t regions{};

    /// Busy time summed over workers.
    [[nodiscard]] Duration busy() const noexcept {
        Duration total{};
        for (const WorkerStatistics& worker : workers) {
            total += worker.busy;
        }
        return total;
    }

    /// Idle time summed over workers.
    [[nodiscard]] Duration idle() const noexcept {
        Duration total{};
        for (const WorkerStatistics& worker : workers) {
            total += worker.idle;
        }
        return total;
    }

    /// The fraction of available worker time that was spent waiting.
    ///
    /// Zero when nothing has run, which is a truthful answer to the question
    /// rather than a division by zero. This is the headline number for the
    /// static against dynamic comparison: it is what a perfectly balanced
    /// scheduler drives towards zero, and what an unbalanced one cannot.
    [[nodiscard]] double idle_fraction() const noexcept {
        const Duration total = busy() + idle();
        if (total.count() == 0) {
            return 0.0;
        }
        return static_cast<double>(idle().count()) / static_cast<double>(total.count());
    }
};

} // namespace orrery::backend
