#include "harness/protocol.hpp"

#include <chrono>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/statistics.hpp"

/// \file
/// That the measurement protocol runs what it says it runs.
///
/// These are not timing tests. A test that asserted a duration would fail on a
/// busy continuous integration machine and would be deleted within a month.
/// What is checked is the structure: that warm-up trials happen and are
/// discarded, that the settling loop terminates rather than always running to
/// its limit, and that the timed trials are counted correctly.

namespace {

using orrery::benchmark::cool_down;
using orrery::benchmark::Duration;
using orrery::benchmark::Protocol;
using orrery::benchmark::run_trials;
using orrery::benchmark::ThermalCanary;
using orrery::benchmark::TrialSet;

} // namespace

TEST_CASE("the timed trials are the ones asked for and the warm-ups are not", "[unit][benchmark]") {
    const Protocol protocol{.warmup = 3, .trials = 5, .cooldown = std::chrono::milliseconds{0}};

    int calls = 0;
    const TrialSet trials = run_trials(protocol, [&] { ++calls; });

    REQUIRE(trials.count() == 5);

    // The warm-up trials and the settling ones ran and were thrown away, so the
    // body was called more often than the set records. How many more is not
    // fixed: settling stops when the machine repeats itself, which is what the
    // bounds below express.
    CAPTURE(calls);
    REQUIRE(calls > protocol.warmup + protocol.trials);
    REQUIRE(calls <= protocol.warmup + protocol.settling_limit + protocol.trials);
}

TEST_CASE("settling stops early rather than always running to its limit", "[unit][benchmark]") {
    // A body too fast for the clock to resolve gives identical durations, which
    // is agreement. Without the special case for it the loop would run its full
    // limit every time, which is both slow and, on a short measurement, a
    // program that spends its time settling rather than measuring.
    const Protocol protocol{.warmup = 0,
                            .settling_limit = 100,
                            .trials = 1,
                            .minimum_trial = Duration{0},
                            .cooldown = std::chrono::milliseconds{0}};

    int calls = 0;
    const TrialSet trials = run_trials(protocol, [&] { ++calls; });

    REQUIRE(trials.count() == 1);

    CAPTURE(calls);
    REQUIRE(calls < 10);
}

TEST_CASE("a protocol with no trials measures nothing without failing", "[unit][benchmark]") {
    const Protocol protocol{.warmup = 0,
                            .settling_limit = 0,
                            .trials = 0,
                            .minimum_trial = Duration{0},
                            .cooldown = std::chrono::milliseconds{0}};

    int calls = 0;
    const TrialSet trials = run_trials(protocol, [&] { ++calls; });

    REQUIRE(trials.empty());
    REQUIRE(calls == 0);
}

TEST_CASE("a body too fast to time is folded into a longer trial", "[unit][benchmark]") {
    // The behaviour the case above switches off. A body the clock can barely
    // resolve is called repeatedly inside one trial and the total divided by
    // the number of calls, so that a short measurement is not dominated by one
    // scheduling event. See `Protocol::minimum_trial`.
    //
    // How many times is deliberately not asserted: it depends on the clock's
    // resolution and on how fast the machine is, which is the point of
    // computing it rather than fixing it. What is asserted is that folding
    // happened at all, and that it did not happen when it was switched off.
    const Protocol folded{.warmup = 0,
                          .trials = 4,
                          .minimum_trial = std::chrono::milliseconds{2},
                          .cooldown = std::chrono::milliseconds{0}};

    int busy_calls = 0;
    const TrialSet busy = run_trials(folded, [&] {
        ++busy_calls;
        // Enough arithmetic that the body takes a measurable but tiny time, so
        // that the harness has something to divide into the minimum.
        volatile double sink = 0;
        for (int step = 0; step < 1000; ++step) {
            sink = sink + 1.0;
        }
    });

    REQUIRE(busy.count() == 4);

    CAPTURE(busy_calls);
    REQUIRE(busy_calls > 4);
}

TEST_CASE("the canary reports nothing until it has been marked twice", "[unit][benchmark]") {
    ThermalCanary canary;

    // One measurement is not a comparison, and a slowdown invented from a
    // single mark would be a number in a report with nothing behind it.
    REQUIRE(canary.marks() == 0);
    REQUIRE(canary.slowdown() == 0.0);

    canary.mark();
    REQUIRE(canary.marks() == 1);
    REQUIRE(canary.slowdown() == 0.0);
    REQUIRE(canary.history().count() == 1);

    canary.mark();
    REQUIRE(canary.marks() == 2);
    REQUIRE(canary.history().count() == 2);

    // The value itself is a property of the machine and is not asserted. What
    // is asserted is that the canary did some work and timed it, rather than
    // recording a zero that would make every later ratio meaningless.
    CAPTURE(canary.slowdown());
    REQUIRE(canary.history().median().count() > 0);
}

TEST_CASE("a cool-down of no time does not sleep", "[unit][benchmark]") {
    // The test suite sets the cool-down to zero throughout, so this is the path
    // every other case in this file takes. It is asserted rather than assumed
    // because a sleep that ignored a zero request would add three quarters of a
    // second to each of them.
    const Protocol protocol{.cooldown = std::chrono::milliseconds{0}};

    const auto start = orrery::benchmark::Clock::now();
    cool_down(protocol);
    const auto elapsed = orrery::benchmark::Clock::now() - start;

    REQUIRE(elapsed < std::chrono::milliseconds{200});
}
