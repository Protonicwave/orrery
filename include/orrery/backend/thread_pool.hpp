#pragma once

/// \file
/// A fixed set of worker threads that run one body function on demand.
///
/// This is the part of the threading that both partitioning schemes share, and
/// it is separated from them so that a comparison between the two measures the
/// partitioning rather than two different thread pools. The static and the
/// work-stealing executors differ in exactly one thing, which is how a worker
/// decides what to do next, and everything else about them is this file.
///
/// The threads are created once and live as long as the pool. Creating a thread
/// costs tens of microseconds on this platform, and a force evaluation at the
/// sizes this project runs at is a few milliseconds, so a pool that spawned per
/// region would spend a noticeable fraction of a run inside the operating
/// system. Between regions the workers sleep on a condition variable rather than
/// spinning. Spinning would shave the wake-up latency off each region at the
/// cost of burning every core between them, which on a laptop means heat and a
/// lower sustained clock, and which would also make the idle time this project
/// set out to measure look like work. The wake-up cost is left in and shows up
/// where it belongs, in the measured idle time.
///
/// The submitting thread does not take a share of the work. It blocks until the
/// region finishes, so the core it was running on is free for a worker, and the
/// alternative would mean either pinning the caller's thread, which is not the
/// pool's to do, or having one worker in the accounting that is not like the
/// others.

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "orrery/backend/cpu_topology.hpp"
#include "orrery/core/function_ref.hpp"

namespace orrery::backend {

/// Worker threads and a way to run something on all of them at once.
class ThreadPool {
public:
    /// Whether workers are tied to a particular logical processor.
    enum class Affinity : std::uint8_t {
        /// Let the operating system place the threads.
        ///
        /// The default, and on this hardware usually the faster of the two:
        /// Windows and Linux both know about hybrid cores and place threads
        /// with information a fixed assignment does not have, including what
        /// else is running on the machine.
        kUnpinned,
        /// Tie worker w to logical processor w.
        ///
        /// A measurement instrument. Attributing idle time to a class of core
        /// requires that a worker stay on one core for the whole region, which
        /// is not otherwise guaranteed.
        kPinned,
    };

    /// What each worker is asked to do, given its own index.
    using WorkerBody = core::FunctionRef<void(unsigned)>;

    /// Start `worker_count` threads, or one per logical processor if zero.
    ///
    /// The constructor returns only once every worker has started and, when
    /// pinned, settled on its processor. Waiting costs a few hundred
    /// microseconds once, and it means the first region measured is not also
    /// measuring thread creation.
    explicit ThreadPool(unsigned worker_count = 0, Affinity affinity = Affinity::kUnpinned);

    /// Stop the workers and join them.
    ~ThreadPool();

    // A pool owns running threads that hold a pointer to it, so it cannot be
    // copied and cannot move out from under them.
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Run `body(worker)` once on every worker and return when all have
    /// returned.
    ///
    /// Call from one thread at a time. The pool is a tool a solver uses to
    /// parallelise its own loop, not a general task queue serving many
    /// submitters, and one submitter is what every caller in this project is.
    ///
    /// `body` must not throw, for the reason `executor.hpp` gives.
    void dispatch(WorkerBody body);

    [[nodiscard]] unsigned worker_count() const noexcept {
        return static_cast<unsigned>(threads_.size());
    }

    [[nodiscard]] Affinity affinity() const noexcept { return affinity_; }

    /// The class of core `worker` runs on, or `kUnknown` when unpinned or
    /// undiscoverable.
    [[nodiscard]] CoreClass core_class(unsigned worker) const noexcept;

    /// One worker per logical processor, and never zero.
    ///
    /// `hardware_concurrency` is permitted to return zero when it cannot tell,
    /// and a pool of no threads would never run anything.
    [[nodiscard]] static unsigned default_worker_count() noexcept;

private:
    void worker_loop(unsigned worker) noexcept;

    std::mutex mutex_;

    /// Signalled when a new body is ready, and when the pool is stopping.
    std::condition_variable work_available_;

    /// Signalled as each worker finishes the current body.
    std::condition_variable work_complete_;

    /// Signalled by each worker once it has started and pinned itself.
    std::condition_variable worker_started_;

    /// The body of the region now running. Owned by the submitting thread,
    /// which outlives the region by construction.
    const WorkerBody* body_{nullptr};

    /// Incremented once per region. A worker compares it against the last value
    /// it saw, so a wake-up that was not a new region is recognised as the
    /// spurious one it is, and a worker that was slow to wake cannot miss a
    /// region and then run the next one twice.
    std::uint64_t generation_{0};

    /// Workers that have not yet finished the current region.
    unsigned pending_{0};

    /// Workers that have not yet reported themselves started.
    unsigned starting_{0};

    bool stopping_{false};

    Affinity affinity_{Affinity::kUnpinned};

    /// The logical processor each worker pins itself to, when pinned. Filled in
    /// by the constructor before any thread starts and not written afterwards.
    std::vector<unsigned> pin_targets_;

    /// The class of core each worker ended up on.
    ///
    /// Written by the workers themselves during startup, and read only after
    /// the constructor has returned. The constructor does not return until
    /// every worker has reported itself started, and it observes that through
    /// the mutex, so those writes are visible to everything that can reach a
    /// constructed pool. That is why reading this needs no lock: after
    /// construction it never changes again.
    std::vector<CoreClass> core_classes_;

    /// Declared last so that every member above is initialised before the first
    /// thread starts and reaches back into the pool.
    std::vector<std::thread> threads_;
};

} // namespace orrery::backend
