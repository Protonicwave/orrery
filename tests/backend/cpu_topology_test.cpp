#include "orrery/backend/cpu_topology.hpp"

#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "orrery/backend/thread_pool.hpp"

namespace {

using orrery::backend::CoreClass;
using orrery::backend::LogicalProcessor;
using orrery::backend::pin_current_thread;
using orrery::backend::query_logical_processors;
using orrery::backend::ThreadPool;
using orrery::backend::to_string;

} // namespace

TEST_CASE("the logical processors are reported without duplicates", "[unit][backend]") {
    // The list is used to decide which processor each worker pins itself to, so
    // a repeated identifier would quietly put two workers on one core and leave
    // another empty. That would look exactly like a load imbalance in the
    // results, which is the thing this phase is trying to measure.
    //
    // An empty list is a valid answer. Not every platform will say, and the
    // header is explicit that an honest absence beats a plausible guess.
    const std::vector<LogicalProcessor> processors = query_logical_processors();

    std::set<unsigned> seen;
    for (const LogicalProcessor& processor : processors) {
        CAPTURE(processor.id, std::string{to_string(processor.core_class)});
        REQUIRE(seen.insert(processor.id).second);
    }

    REQUIRE(seen.size() == processors.size());
}

TEST_CASE("a hybrid machine reports both kinds of core", "[unit][backend]") {
    // Not an assertion about this machine, because the test suite has to pass on
    // continuous integration runners that are uniformly one kind of core. It is
    // an assertion about the classification: the two classes are reported
    // together or not at all. A list that claimed performance cores and no
    // efficiency cores would mean the ranking had been misread, since the class
    // is assigned by comparison against the fastest core present and something
    // is always the fastest.
    const std::vector<LogicalProcessor> processors = query_logical_processors();

    unsigned performance = 0;
    unsigned efficiency = 0;
    unsigned unknown = 0;

    for (const LogicalProcessor& processor : processors) {
        switch (processor.core_class) {
        case CoreClass::kPerformance:
            performance += 1;
            break;
        case CoreClass::kEfficiency:
            efficiency += 1;
            break;
        case CoreClass::kUnknown:
            unknown += 1;
            break;
        }
    }

    CAPTURE(processors.size(), performance, efficiency, unknown);

    const bool hybrid = performance > 0 || efficiency > 0;
    REQUIRE((!hybrid || (performance > 0 && efficiency > 0)));
    REQUIRE((!hybrid || unknown == 0));
}

TEST_CASE("core class names are distinct", "[unit][backend]") {
    // They appear in the tables in docs/performance/, where two classes sharing
    // a label would make the report unreadable in the one dimension it exists
    // to report.
    REQUIRE(std::string{to_string(CoreClass::kPerformance)} == "performance");
    REQUIRE(std::string{to_string(CoreClass::kEfficiency)} == "efficiency");
    REQUIRE(std::string{to_string(CoreClass::kUnknown)} == "unknown");
}

TEST_CASE("pinning leaves the calling thread alone", "[unit][backend]") {
    // Run on a thread of its own rather than on the test runner's. Pinning is
    // not reversible through this interface, and confining Catch2's own thread
    // to one core for the remainder of the suite would slow every test after
    // this one and quietly distort the pool tests.
    bool completed = false;

    std::thread worker{[&completed] {
        // The return value is not asserted on. Whether a platform allows
        // pinning is a property of the platform, and macOS deliberately refuses,
        // so a test that required success would fail on a machine behaving
        // correctly. What is being checked is that the call is safe to make
        // anywhere, which is what the pool relies on.
        (void)pin_current_thread(0);
        completed = true;
    }};

    worker.join();
    REQUIRE(completed);
}

TEST_CASE("an unpinned pool claims to know nothing about its cores", "[unit][backend]") {
    // The distinction the header insists on. Without pinning the operating
    // system may move a worker between a performance and an efficiency core in
    // the middle of a region, so any per-core-class figure taken from an
    // unpinned pool would be an assertion about where a thread started rather
    // than about where it did its work.
    ThreadPool pool{2};

    REQUIRE(pool.affinity() == ThreadPool::Affinity::kUnpinned);
    REQUIRE(pool.core_class(0) == CoreClass::kUnknown);
    REQUIRE(pool.core_class(1) == CoreClass::kUnknown);
}

TEST_CASE("a pinned pool reports where its workers went", "[unit][backend]") {
    const std::vector<LogicalProcessor> processors = query_logical_processors();

    if (processors.size() < 2) {
        SUCCEED("This platform does not report its topology, so there is nothing to attribute");
        return;
    }

    ThreadPool pool{2, ThreadPool::Affinity::kPinned};
    REQUIRE(pool.affinity() == ThreadPool::Affinity::kPinned);

    for (unsigned worker = 0; worker < 2; ++worker) {
        const CoreClass reported = pool.core_class(worker);

        CAPTURE(worker, std::string{to_string(reported)},
                std::string{to_string(processors[worker].core_class)});

        // Either the worker pinned itself to the processor it was assigned and
        // reports that processor's class, or pinning was refused and it reports
        // that it no longer knows. What it must never do is report a class it
        // has no reason to believe.
        REQUIRE((reported == processors[worker].core_class || reported == CoreClass::kUnknown));
    }

    // A worker index beyond the pool is not a core class of its own.
    REQUIRE(pool.core_class(99) == CoreClass::kUnknown);
}
