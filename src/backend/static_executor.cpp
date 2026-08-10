#include "orrery/backend/static_executor.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

#include "orrery/backend/executor.hpp"
#include "orrery/backend/partition.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/types.hpp"

namespace orrery::backend {

StaticExecutor::StaticExecutor(unsigned worker_count, ThreadPool::Affinity affinity)
    : pool_(worker_count, affinity),
      workers_(pool_.worker_count()),
      busy_at_region_start_(pool_.worker_count()) {}

void StaticExecutor::run(core::Index count, RangeTask task) {
    if (count == 0) {
        return;
    }

    const unsigned workers = pool_.worker_count();

    for (unsigned worker = 0; worker < workers; ++worker) {
        busy_at_region_start_[worker] = workers_[worker].value.busy;
    }

    const Clock::time_point start = Clock::now();

    pool_.dispatch([&](unsigned worker) noexcept {
        const IndexRange share = equal_share(count, workers, worker);
        if (share.empty()) {
            // More workers than cache lines of work. The surplus workers still
            // report the region as idle time, which is the truthful account of
            // a pool larger than the problem.
            return;
        }

        WorkerStatistics& statistics = workers_[worker].value;

        const Clock::time_point chunk_start = Clock::now();
        task(share.begin, share.end);
        statistics.busy += std::chrono::duration_cast<Duration>(Clock::now() - chunk_start);

        statistics.chunks += 1;
        statistics.items += static_cast<std::uint64_t>(share.size());
    });

    const auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);

    elapsed_ += elapsed;
    ++regions_;

    for (unsigned worker = 0; worker < workers; ++worker) {
        const Duration busy = workers_[worker].value.busy - busy_at_region_start_[worker];

        // A worker's busy time is bracketed by two readings it took itself,
        // and the region is bracketed by two the submitting thread took, so
        // a worker that started fractionally before the outer reading can
        // report marginally more busy time than the region lasted. The
        // difference is a clock tick and the clamp keeps it from becoming a
        // negative idle time that would then be summed into a total.
        workers_[worker].value.idle += elapsed > busy ? elapsed - busy : Duration::zero();
    }
}

ExecutorStatistics StaticExecutor::statistics() const {
    ExecutorStatistics result;
    result.workers.reserve(workers_.size());
    for (const PaddedWorkerStatistics& worker : workers_) {
        result.workers.push_back(worker.value);
    }
    result.elapsed = elapsed_;
    result.regions = regions_;
    return result;
}

void StaticExecutor::reset_statistics() noexcept {
    for (PaddedWorkerStatistics& worker : workers_) {
        worker.value = WorkerStatistics{};
    }
    elapsed_ = Duration::zero();
    regions_ = 0;
}

} // namespace orrery::backend
