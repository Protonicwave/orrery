#include "orrery/backend/thread_pool.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "orrery/backend/cpu_topology.hpp"

namespace orrery::backend {

ThreadPool::ThreadPool(unsigned worker_count, Affinity affinity) : affinity_(affinity) {
    const unsigned count = worker_count == 0 ? default_worker_count() : worker_count;

    core_classes_.assign(count, CoreClass::kUnknown);

    if (affinity_ == Affinity::kPinned) {
        const std::vector<LogicalProcessor> processors = query_logical_processors();

        // Worker w takes processor w. The platform's own ordering is used
        // rather than one sorted by core class, so that a pool of fewer workers
        // than processors lands wherever the operating system numbers them
        // first, and the report says which class each worker actually got
        // instead of assuming a layout.
        pin_targets_.reserve(processors.size());
        for (unsigned worker = 0; worker < count && worker < processors.size(); ++worker) {
            pin_targets_.push_back(processors[worker].id);
            core_classes_[worker] = processors[worker].core_class;
        }
    }

    // Set before any thread starts, so that the first worker to reach the
    // barrier below finds a count that already includes all of its peers.
    starting_ = count;

    threads_.reserve(count);
    for (unsigned worker = 0; worker < count; ++worker) {
        threads_.emplace_back([this, worker] { worker_loop(worker); });
    }

    std::unique_lock lock{mutex_};
    worker_started_.wait(lock, [this] { return starting_ == 0; });
}

ThreadPool::~ThreadPool() {
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
    }

    // Every worker is either waiting on this variable or about to wait on it,
    // and the flag is set under the same mutex the wait predicate reads, so
    // none can miss the signal and sleep through the join below.
    work_available_.notify_all();

    for (std::thread& thread : threads_) {
        thread.join();
    }
}

void ThreadPool::dispatch(WorkerBody body) {
    if (threads_.empty()) {
        return;
    }

    {
        const std::scoped_lock lock{mutex_};

        // The address of the caller's reference. It stays valid for the whole
        // region because `dispatch` does not return until every worker has
        // finished with it.
        body_ = &body;
        pending_ = worker_count();
        ++generation_;
    }

    work_available_.notify_all();

    std::unique_lock lock{mutex_};
    work_complete_.wait(lock, [this] { return pending_ == 0; });
    body_ = nullptr;
}

void ThreadPool::worker_loop(unsigned worker) noexcept {
    if (affinity_ == Affinity::kPinned) {
        const bool pinned =
            worker < pin_targets_.size() && pin_current_thread(pin_targets_[worker]);

        if (!pinned) {
            // The pool still runs, on whichever core the operating system
            // chooses. What it must not do is go on reporting a core class it
            // no longer has any reason to believe, because a per-core-class
            // figure derived from that would be fiction.
            const std::scoped_lock lock{mutex_};
            core_classes_[worker] = CoreClass::kUnknown;
        }
    }

    {
        const std::scoped_lock lock{mutex_};
        --starting_;
    }
    worker_started_.notify_one();

    // The generation this worker has already run. Zero is the value the pool
    // starts at, so a worker that has run nothing agrees with a pool that has
    // dispatched nothing.
    std::uint64_t seen = 0;

    while (true) {
        const WorkerBody* body = nullptr;

        {
            std::unique_lock lock{mutex_};
            work_available_.wait(lock, [this, seen] { return stopping_ || generation_ != seen; });

            if (stopping_) {
                return;
            }

            seen = generation_;
            body = body_;
        }

        // Outside the lock, which is the point of the whole arrangement: the
        // workers hold no shared state while they are working. Acquiring the
        // mutex above also published everything the submitting thread wrote
        // before it incremented the generation, so the body and whatever it
        // captured are visible here without any further synchronisation.
        (*body)(worker);

        {
            const std::scoped_lock lock{mutex_};
            --pending_;
        }
        work_complete_.notify_one();
    }
}

CoreClass ThreadPool::core_class(unsigned worker) const noexcept {
    if (worker >= core_classes_.size()) {
        return CoreClass::kUnknown;
    }
    return core_classes_[worker];
}

unsigned ThreadPool::default_worker_count() noexcept {
    const unsigned reported = std::thread::hardware_concurrency();
    return reported == 0 ? 1U : reported;
}

} // namespace orrery::backend
