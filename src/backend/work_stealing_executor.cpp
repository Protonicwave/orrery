#include "orrery/backend/work_stealing_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/partition.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

WorkStealingExecutor::WorkStealingExecutor(unsigned worker_count, ThreadPool::Affinity affinity)
    : pool_(worker_count, affinity),
      workers_(pool_.worker_count()),
      busy_at_region_start_(pool_.worker_count()),
      ranges_(pool_.worker_count()) {}

void WorkStealingExecutor::run(core::Index count, RangeTask task) {
    if (count == 0) {
        return;
    }

    const unsigned workers = pool_.worker_count();

    // Chunks sized so that each worker's share breaks into about
    // kChunksPerWorker of them, rounded to whole cache lines so that no two
    // workers ever write to the same line. At least one line, because a chunk
    // of nothing would not make progress.
    const core::Index lines = (count + kPartitionGrain - 1) / kPartitionGrain;
    const core::Index wanted_chunks = static_cast<core::Index>(workers) * kChunksPerWorker;
    grain_ =
        std::max<core::Index>(1, lines / std::max<core::Index>(1, wanted_chunks)) * kPartitionGrain;

    for (unsigned worker = 0; worker < workers; ++worker) {
        const IndexRange share = equal_share(count, workers, worker);
        ranges_[worker].next = share.begin;
        ranges_[worker].end = share.end;
        busy_at_region_start_[worker] = workers_[worker].value.busy;
    }

    task_ = &task;

    const Clock::time_point start = Clock::now();

    // The ranges, the grain and the task pointer are all written above and read
    // by the workers below. The pool's own mutex orders the two: a worker
    // acquires it to observe the new generation, and the submitting thread
    // released it after these writes.
    pool_.dispatch([this](unsigned worker) noexcept { drain(worker); });

    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    task_ = nullptr;
    elapsed_ += elapsed;
    ++regions_;

    for (unsigned worker = 0; worker < workers; ++worker) {
        const Duration busy = workers_[worker].value.busy - busy_at_region_start_[worker];
        // Clamped for the reason the static executor gives.
        workers_[worker].value.idle += elapsed > busy ? elapsed - busy : Duration::zero();
    }
}

void WorkStealingExecutor::drain(unsigned worker) noexcept {
    WorkerStatistics& statistics = workers_[worker].value;

    IndexRange chunk{};

    while (true) {
        bool stolen = false;

        if (!claim_front(worker, chunk)) {
            if (!steal(worker, chunk)) {
                // Nothing here and nothing anywhere. No work can appear after
                // this point in a region, so there is nothing to wait for.
                return;
            }
            stolen = true;
        }

        const Clock::time_point chunk_start = Clock::now();
        (*task_)(chunk.begin, chunk.end);
        statistics.busy += std::chrono::duration_cast<Duration>(Clock::now() - chunk_start);

        statistics.chunks += 1;
        statistics.items += static_cast<std::uint64_t>(chunk.size());
        if (stolen) {
            statistics.steals += 1;
        }
    }
}

bool WorkStealingExecutor::claim_front(unsigned worker, IndexRange& chunk) noexcept {
    WorkRange& range = ranges_[worker];
    const std::scoped_lock lock{range.mutex};

    if (range.next >= range.end) {
        return false;
    }

    // Forwards from where the owner left off, so that its reads walk the
    // component arrays in the one direction the hardware prefetcher follows.
    chunk.begin = range.next;
    chunk.end = std::min(range.next + grain_, range.end);
    range.next = chunk.end;
    return true;
}

bool WorkStealingExecutor::claim_back(unsigned victim, IndexRange& chunk) noexcept {
    WorkRange& range = ranges_[victim];
    const std::scoped_lock lock{range.mutex};

    if (range.next >= range.end) {
        return false;
    }

    // From the far end, so that a thief and its victim are working on opposite
    // ends of the same share and do not touch the same cache lines until the
    // share is nearly exhausted. Taking no more than what is left keeps the two
    // ends from crossing, which is the whole of the invariant this lock exists
    // to hold.
    const core::Index available = range.end - range.next;
    const core::Index taken = std::min(grain_, available);

    chunk.end = range.end;
    chunk.begin = range.end - taken;
    range.end = chunk.begin;
    return true;
}

bool WorkStealingExecutor::steal(unsigned thief, IndexRange& chunk) noexcept {
    const unsigned workers = pool_.worker_count();

    // Round robin from the next worker along rather than at random. It needs no
    // random source, it makes a run reproducible, and it spreads the thieves
    // out: two workers that run dry at the same moment start looking in
    // different places instead of contending on the same victim's lock.
    for (unsigned offset = 1; offset < workers; ++offset) {
        const unsigned victim = (thief + offset) % workers;
        if (claim_back(victim, chunk)) {
            return true;
        }
    }

    return false;
}

ExecutorStatistics WorkStealingExecutor::statistics() const {
    ExecutorStatistics result;
    result.workers.reserve(workers_.size());
    for (const PaddedWorkerStatistics& worker : workers_) {
        result.workers.push_back(worker.value);
    }
    result.elapsed = elapsed_;
    result.regions = regions_;
    return result;
}

void WorkStealingExecutor::reset_statistics() noexcept {
    for (PaddedWorkerStatistics& worker : workers_) {
        worker.value = WorkerStatistics{};
    }
    elapsed_ = Duration::zero();
    regions_ = 0;
}

} // namespace orrery::backend
