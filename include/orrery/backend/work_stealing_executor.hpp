#pragma once

/// \file
/// Dynamic partitioning by range stealing. The scheme this project uses.
///
/// The range is divided into equal shares exactly as the static scheme divides
/// it, and each worker starts on its own. The difference is what happens when a
/// worker runs out. Instead of stopping, it looks at the other workers' shares
/// and takes work from one that still has some. A performance core that finishes
/// early therefore spends the time it would have spent waiting doing an
/// efficiency core's work instead, and the region ends when the work is gone
/// rather than when the slowest worker's fixed share is gone.
///
/// Nothing in this needs to know which cores are fast. That is the property
/// worth having. A scheme that split the range four-to-one because the
/// performance cores are about twice the throughput of the efficiency ones
/// would need a ratio, the ratio would have to be measured, and it would then be
/// wrong whenever the machine was thermally limited, running on battery, sharing
/// the memory bus with the integrated GPU, or executing a kernel with a
/// different arithmetic mix. Stealing measures the ratio implicitly, continuously
/// and for free.
///
/// ## Why stealing ranges rather than a task deque
///
/// The textbook work-stealing scheduler is Chase and Lev's lock-free deque, and
/// it is the right structure for a scheduler that has to support nested spawning
/// and arbitrary task graphs. This one does not: every parallel region here is a
/// flat loop over particle indices whose extent is known before it starts, which
/// is a much weaker problem. So each worker owns a half-open range rather than a
/// queue of tasks, takes chunks from the front of its own, and steals from the
/// back of a victim's, and the whole of the concurrency is two indices that must
/// not cross.
///
/// Those two indices are protected by an ordinary mutex rather than by atomics.
/// That deserves stating plainly, because a lock in a work-stealing scheduler
/// looks like a mistake. It is on no hot path. A chunk is sized at roughly a
/// sixteenth of a worker's share, so a lock is taken a few tens of times per
/// worker per region while each chunk is tens of microseconds of arithmetic:
/// the lock is well under a thousandth of the run time, and it is uncontended
/// almost always, since a thief only reaches a victim's mutex after it has run
/// out of its own work. Against that, the lock-free protocol for a range that
/// can be claimed from both ends has a genuinely subtle race where the two ends
/// meet, and its correctness argument lives in a paper rather than in the file.
/// Section 5 of the implementation plan asks that any single file be defensible
/// to a reviewer who did not write it, and this is what that costs here.
/// ADR-0016 records the decision.
///
/// ## Termination
///
/// A worker stops when its own range is empty and a full sweep of every other
/// worker's range finds nothing to take. That is safe because work is only ever
/// removed from a range, never added: no region spawns new work, so a range
/// observed empty stays empty for the rest of the region. Work already claimed
/// by another worker is not lost by this worker exiting, because the claimer
/// runs it before checking again, and the pool does not consider the region
/// finished until every worker has returned.

#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include "orrery/backend/cpu_topology.hpp"
#include "orrery/backend/executor.hpp"
#include "orrery/backend/partition.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/aligned_allocator.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

/// Divides the range into equal shares and lets idle workers take from busy
/// ones.
class WorkStealingExecutor final : public Executor {
public:
    /// How many chunks each worker's share is broken into.
    ///
    /// This is the one tuning constant in the scheduler and it is a compromise
    /// between two costs. Larger chunks mean fewer lock acquisitions and fewer
    /// clock readings, but a coarser tail: the region cannot finish sooner than
    /// the last chunk started, so the residual imbalance is about one chunk.
    /// Smaller chunks balance more finely and cost more to hand out.
    ///
    /// Sixteen puts the residual imbalance at roughly one part in sixteen of a
    /// worker's share, which is well under the two-to-one throughput difference
    /// between the core types this is here to absorb, while keeping the
    /// per-chunk overhead near a thousandth of the chunk. The value is not
    /// finely tuned and does not need to be: anything from about eight to
    /// sixty-four behaves the same on this machine, which is the sign of a
    /// constant sitting in the flat part of the curve rather than on a peak.
    static constexpr core::Index kChunksPerWorker = 16;

    explicit WorkStealingExecutor(unsigned worker_count = 0,
                                  ThreadPool::Affinity affinity = ThreadPool::Affinity::kUnpinned);

    void run(core::Index count, RangeTask task) override;

    [[nodiscard]] std::string_view name() const noexcept override { return "work-stealing"; }

    [[nodiscard]] unsigned worker_count() const noexcept override { return pool_.worker_count(); }

    [[nodiscard]] CoreClass worker_core_class(unsigned worker) const noexcept override {
        return pool_.core_class(worker);
    }

    [[nodiscard]] ExecutorStatistics statistics() const override;

    void reset_statistics() noexcept override;

private:
    /// One worker's remaining share, claimable from either end.
    ///
    /// On its own cache line for the reason `PaddedWorkerStatistics` is: a
    /// thief taking the lock of one range must not invalidate the line holding
    /// the range its owner is working from.
    struct alignas(core::kCacheLineBytes) WorkRange {
        std::mutex mutex;

        /// The next index the owner will take. Only the owner advances it.
        core::Index next{};

        /// One past the last index in the range. Only a thief moves it, and
        /// only downwards.
        core::Index end{};
    };

    /// Run chunks until there are none left anywhere.
    void drain(unsigned worker) noexcept;

    /// Take a chunk from the front of a worker's own range.
    [[nodiscard]] bool claim_front(unsigned worker, IndexRange& chunk) noexcept;

    /// Take a chunk from the back of a victim's range.
    [[nodiscard]] bool claim_back(unsigned victim, IndexRange& chunk) noexcept;

    /// Try every other worker in turn until one yields a chunk.
    [[nodiscard]] bool steal(unsigned thief, IndexRange& chunk) noexcept;

    ThreadPool pool_;

    std::vector<PaddedWorkerStatistics> workers_;
    std::vector<Duration> busy_at_region_start_;

    /// One per worker. Sized once at construction and never resized, which is
    /// what lets it hold a type containing a mutex.
    std::vector<WorkRange> ranges_;

    /// The work of the region now running, owned by the submitting thread.
    const RangeTask* task_{nullptr};

    /// Chunk size for the current region, a whole number of cache lines.
    core::Index grain_{kPartitionGrain};

    Duration elapsed_{};
    std::uint64_t regions_{};
};

} // namespace orrery::backend
