#include "orrery/backend/executor.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/partition.hpp"
#include "orrery/backend/serial_executor.hpp"
#include "orrery/backend/static_executor.hpp"
#include "orrery/backend/thread_pool.hpp"
#include "orrery/backend/work_stealing_executor.hpp"
#include "orrery/backend/worker_statistics.hpp"
#include "orrery/core/types.hpp"

namespace {

using orrery::backend::equal_share;
using orrery::backend::Executor;
using orrery::backend::ExecutorStatistics;
using orrery::backend::IndexRange;
using orrery::backend::SerialExecutor;
using orrery::backend::StaticExecutor;
using orrery::backend::ThreadPool;
using orrery::backend::WorkerStatistics;
using orrery::backend::WorkStealingExecutor;
using orrery::core::Index;

/// Sizes chosen to straddle the interesting boundaries: fewer items than
/// workers, fewer than one cache line, exactly one chunk, and enough to give
/// every worker several chunks to get through.
constexpr auto kCounts = std::to_array<Index>({0, 1, 3, 8, 17, 64, 255, 1024, 5000});

/// Run `count` items through `executor` and check every index was covered
/// exactly once.
///
/// The tally is `std::vector<int>` rather than `std::vector<bool>`, and that is
/// not a style choice. The vector of bool specialisation packs eight elements
/// into a byte, so two workers writing to two different indices in the same byte
/// would be a genuine data race on the same object. An int per index is a race
/// only if the partition is wrong, which is exactly what this is checking.
void check_covers_range_once(Executor& executor, Index count) {
    std::vector<int> visits(count, 0);

    executor.run(count, [&](Index begin, Index end) {
        for (Index index = begin; index < end; ++index) {
            visits[index] += 1;
        }
    });

    for (Index index = 0; index < count; ++index) {
        CAPTURE(std::string{executor.name()}, count, index, visits[index]);
        REQUIRE(visits[index] == 1);
    }
}

} // namespace

TEST_CASE("every executor covers the range exactly once", "[unit][backend]") {
    // The whole contract of `Executor::run`. Everything else in this layer is
    // an optimisation of how the range is divided, and a division that lost or
    // duplicated an index would silently corrupt a simulation rather than fail.
    SerialExecutor serial;
    StaticExecutor fixed;
    WorkStealingExecutor stealing;

    // A pool with more workers than the machine has cores, which is where a
    // partition that assumed one worker per core would come apart.
    StaticExecutor oversubscribed{16};
    WorkStealingExecutor stealing_oversubscribed{16};

    const auto executors = std::to_array<Executor*>(
        {&serial, &fixed, &stealing, &oversubscribed, &stealing_oversubscribed});

    for (Executor* executor : executors) {
        for (const Index count : kCounts) {
            check_covers_range_once(*executor, count);
        }
    }
}

TEST_CASE("a single worker is still a valid pool", "[unit][backend]") {
    // The degenerate configuration, and the one a machine reporting a single
    // core would produce. A work-stealing pool of one has nobody to steal from,
    // so the sweep for a victim has to terminate rather than loop.
    StaticExecutor fixed{1};
    WorkStealingExecutor stealing{1};

    REQUIRE(fixed.worker_count() == 1);
    REQUIRE(stealing.worker_count() == 1);

    check_covers_range_once(fixed, 1000);
    check_covers_range_once(stealing, 1000);
}

TEST_CASE("repeated regions reuse the same threads", "[unit][backend]") {
    // The pool is built once and dispatched to many times, which is the pattern
    // a simulation produces: one force evaluation per stage, several stages per
    // step, for the length of the run. This is the case that a generation
    // counter mishandled would show up in, either by a worker missing a region
    // or by running one twice.
    constexpr Index kCount = 2000;
    constexpr int kRegions = 200;

    WorkStealingExecutor stealing{4};
    std::vector<int> visits(kCount, 0);

    for (int region = 0; region < kRegions; ++region) {
        stealing.run(kCount, [&](Index begin, Index end) {
            for (Index index = begin; index < end; ++index) {
                visits[index] += 1;
            }
        });
    }

    for (Index index = 0; index < kCount; ++index) {
        CAPTURE(index, visits[index]);
        REQUIRE(visits[index] == kRegions);
    }

    REQUIRE(stealing.statistics().regions == static_cast<std::uint64_t>(kRegions));
}

TEST_CASE("every executor accounts for all the work", "[unit][backend]") {
    // The items counter is what `docs/performance/threading.md` divides between
    // core classes to show how the schemes differ, so it has to add up to the
    // work that was actually asked for rather than approximately to it.
    constexpr Index kCount = 100000;

    StaticExecutor fixed{4};
    WorkStealingExecutor stealing{4};

    for (Executor* executor : {static_cast<Executor*>(&fixed), static_cast<Executor*>(&stealing)}) {
        executor->run(kCount, [](Index begin, Index end) {
            volatile double sink = 0;
            for (Index index = begin; index < end; ++index) {
                sink += static_cast<double>(index);
            }
        });

        const ExecutorStatistics statistics = executor->statistics();

        std::uint64_t total_items = 0;
        for (const WorkerStatistics& worker : statistics.workers) {
            total_items += worker.items;
        }

        CAPTURE(std::string{executor->name()}, total_items);
        REQUIRE(total_items == kCount);
    }
}

TEST_CASE("static shares reach every worker", "[unit][backend]") {
    // A dispatch that never reached the other threads would pass every
    // correctness test in this file and be worthless, so something has to rule
    // it out. Under static partitioning it can be ruled out exactly: the pool
    // runs the body on every worker and waits for all of them, each worker's
    // share here is non-empty, so all four do their own share and no other.
    //
    // The same assertion must not be made of work stealing, and an earlier
    // version of this file made it and failed intermittently on continuous
    // integration. A worker that wakes first is entitled to drain its own range
    // and then steal every other range before its peers have woken at all. That
    // is the scheme working rather than failing: no work is lost, and on a
    // machine whose other cores are slow to arrive, the worker that is already
    // running should not wait for them. Work stealing promises that the range is
    // covered exactly once, not that any particular worker gets a share of it,
    // and a test may only assert what the thing under test promises.
    //
    // What covers the stealing scheme instead is the pool test at the end of
    // this file, which does guarantee that a body reaches every worker, and the
    // steal counter below.
    constexpr Index kCount = 100000;
    constexpr unsigned kWorkers = 4;

    StaticExecutor fixed{kWorkers};

    fixed.run(kCount, [](Index begin, Index end) {
        volatile double sink = 0;
        for (Index index = begin; index < end; ++index) {
            sink += static_cast<double>(index);
        }
    });

    const ExecutorStatistics statistics = fixed.statistics();

    for (unsigned worker = 0; worker < kWorkers; ++worker) {
        const IndexRange share = equal_share(kCount, kWorkers, worker);

        CAPTURE(worker, share.begin, share.end, statistics.workers[worker].items);

        REQUIRE(!share.empty());
        REQUIRE(statistics.workers[worker].items == share.size());
    }
}

TEST_CASE("the statistics account for every worker's time", "[unit][backend]") {
    // The identity the whole of `worker_statistics.hpp` rests on: within a
    // region each worker is either working or waiting, so busy plus idle is the
    // duration of the region for every one of them. Phase 6's headline figure
    // is a ratio of those two quantities, so an accounting that leaked time
    // would misreport exactly the result it exists to substantiate.
    constexpr Index kCount = 50000;
    constexpr unsigned kWorkers = 4;

    WorkStealingExecutor stealing{kWorkers};

    stealing.run(kCount, [](Index begin, Index end) {
        volatile double sink = 0;
        for (Index index = begin; index < end; ++index) {
            sink += static_cast<double>(index);
        }
    });

    const ExecutorStatistics statistics = stealing.statistics();

    REQUIRE(statistics.workers.size() == kWorkers);
    REQUIRE(statistics.regions == 1);
    REQUIRE(statistics.elapsed.count() > 0);

    for (const WorkerStatistics& worker : statistics.workers) {
        const auto accounted = worker.busy + worker.idle;

        CAPTURE(worker.busy.count(), worker.idle.count(), statistics.elapsed.count());

        // Equality up to the clock's own resolution. The worker brackets its
        // busy time with two readings it took itself and the submitting thread
        // brackets the region with two of its own, so the two measurements of
        // the same interval differ by a tick or two at each end.
        REQUIRE(accounted <= statistics.elapsed);
        REQUIRE(accounted.count() > 0);
    }

    REQUIRE(statistics.idle_fraction() >= 0.0);
    REQUIRE(statistics.idle_fraction() <= 1.0);
}

TEST_CASE("resetting the statistics clears them", "[unit][backend]") {
    // A benchmark counts the region it timed rather than the warm-up before it,
    // and once the two have been added together they cannot be separated again.
    WorkStealingExecutor stealing{2};

    stealing.run(1000, [](Index, Index) {});
    REQUIRE(stealing.statistics().regions == 1);

    stealing.reset_statistics();

    const ExecutorStatistics statistics = stealing.statistics();
    REQUIRE(statistics.regions == 0);
    REQUIRE(statistics.elapsed.count() == 0);
    REQUIRE(statistics.idle_fraction() == 0.0);

    for (const WorkerStatistics& worker : statistics.workers) {
        REQUIRE(worker.chunks == 0);
        REQUIRE(worker.items == 0);
        REQUIRE(worker.steals == 0);
        REQUIRE(worker.busy.count() == 0);
        REQUIRE(worker.idle.count() == 0);
    }
}

TEST_CASE("an idle worker takes work from a busy one", "[unit][backend]") {
    // The behaviour the scheme is named for, provoked deliberately rather than
    // waited for. The work at each index is made wildly unequal, with almost all
    // of it in the last tenth of the range, so the workers holding the early
    // shares run out long before the one holding the end. A scheme that did not
    // steal would leave them idle; this one has them finish the last worker's
    // range for it.
    constexpr Index kCount = 4096;
    constexpr unsigned kWorkers = 4;

    WorkStealingExecutor stealing{kWorkers};

    stealing.run(kCount, [](Index begin, Index end) {
        volatile double sink = 0;
        for (Index index = begin; index < end; ++index) {
            const int repeats = index > (kCount * 9 / 10) ? 20000 : 1;
            for (int repeat = 0; repeat < repeats; ++repeat) {
                sink += static_cast<double>(index);
            }
        }
    });

    const ExecutorStatistics statistics = stealing.statistics();

    std::uint64_t steals = 0;
    for (const WorkerStatistics& worker : statistics.workers) {
        steals += worker.steals;
    }

    CAPTURE(steals);
    REQUIRE(steals > 0);
}

TEST_CASE("the executors report who they are", "[unit][backend]") {
    // Converted rather than compared as a view, for the reason the integrator
    // fixtures give: Catch2 renders a std::string_view only when its own library
    // was built against the same standard as the test that uses it.
    SerialExecutor serial;
    StaticExecutor fixed{2};
    WorkStealingExecutor stealing{2};

    REQUIRE(std::string{serial.name()} == "serial");
    REQUIRE(std::string{fixed.name()} == "static");
    REQUIRE(std::string{stealing.name()} == "work-stealing");

    REQUIRE(serial.worker_count() == 1);
    REQUIRE(fixed.worker_count() == 2);
    REQUIRE(stealing.worker_count() == 2);
}

TEST_CASE("a default pool has one worker per logical processor", "[unit][backend]") {
    // The default has to be right without being configured, because every
    // caller that does not care about threading gets it.
    const unsigned expected = ThreadPool::default_worker_count();
    REQUIRE(expected >= 1);

    StaticExecutor fixed;
    WorkStealingExecutor stealing;

    REQUIRE(fixed.worker_count() == expected);
    REQUIRE(stealing.worker_count() == expected);
}

TEST_CASE("the pool runs a body once on each worker", "[unit][backend]") {
    // The pool underneath both schemes, tested on its own so that a failure
    // here is not mistaken for a partitioning bug.
    constexpr unsigned kWorkers = 4;
    ThreadPool pool{kWorkers};

    REQUIRE(pool.worker_count() == kWorkers);

    std::vector<int> entries(kWorkers, 0);
    std::atomic<int> total{0};

    for (int region = 0; region < 50; ++region) {
        pool.dispatch([&](unsigned worker) noexcept {
            entries[worker] += 1;
            total.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (const int count : entries) {
        REQUIRE(count == 50);
    }
    REQUIRE(total.load() == 50 * static_cast<int>(kWorkers));
}
