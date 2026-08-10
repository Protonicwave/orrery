#include "harness/statistics.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

/// \file
/// The statistics a performance claim rests on.
///
/// Everything Phase 7 reports goes through `TrialSet`, so an error here would
/// not produce a wrong program, it would produce wrong numbers in
/// `docs/performance/` with nothing to catch them. The timings are written down
/// by hand rather than measured, which is what makes the expected answers
/// arithmetic rather than opinion.

namespace {

using orrery::benchmark::Duration;
using orrery::benchmark::rate_per_second;
using orrery::benchmark::TrialSet;

[[nodiscard]] Duration ms(long long count) {
    return std::chrono::duration_cast<Duration>(std::chrono::milliseconds{count});
}

/// Trials in the order given, which for the drift tests below is the point.
[[nodiscard]] TrialSet trials_of(const std::vector<long long>& milliseconds) {
    std::vector<Duration> timings;
    timings.reserve(milliseconds.size());
    for (const long long value : milliseconds) {
        timings.push_back(ms(value));
    }
    return TrialSet{std::move(timings)};
}

} // namespace

TEST_CASE("the order statistics are trials that happened", "[unit][benchmark]") {
    // Deliberately out of order, because a set that arrived sorted would not
    // distinguish an implementation that sorts from one that assumes.
    const TrialSet trials = trials_of({50, 10, 30, 20, 40});

    REQUIRE(trials.count() == 5);
    REQUIRE_FALSE(trials.empty());

    REQUIRE(trials.median() == ms(30));
    REQUIRE(trials.fastest() == ms(10));
    REQUIRE(trials.slowest() == ms(50));

    // Nearest rank, not interpolated: every figure reported is one of the five
    // numbers above rather than a weighted average of two of them.
    REQUIRE(trials.lower_quartile() == ms(20));
    REQUIRE(trials.upper_quartile() == ms(40));

    // The interquartile range over the median, which for these numbers is
    // (40 - 20) / 30.
    REQUIRE(trials.relative_spread() > 0.66);
    REQUIRE(trials.relative_spread() < 0.67);
}

TEST_CASE("an empty set of trials reports zeros rather than dividing by none",
          "[unit][benchmark]") {
    // A benchmark that measured nothing should print zeros. This is not a
    // hypothetical: a configuration skipped because the machine lacks the
    // kernel it wanted reaches the printing code with no trials in it.
    const TrialSet trials;

    REQUIRE(trials.empty());
    REQUIRE(trials.count() == 0);
    REQUIRE(trials.median() == Duration{});
    REQUIRE(trials.fastest() == Duration{});
    REQUIRE(trials.relative_spread() == 0.0);
    REQUIRE(trials.drift() == 0.0);
}

TEST_CASE("a single trial has no dispersion and no drift", "[unit][benchmark]") {
    const TrialSet trials = trials_of({42});

    REQUIRE(trials.median() == ms(42));
    REQUIRE(trials.relative_spread() == 0.0);

    // Two halves of half a trial each say nothing, so the honest answer is
    // zero rather than a number computed from one measurement.
    REQUIRE(trials.drift() == 0.0);
}

TEST_CASE("drift sees a machine slowing down that dispersion cannot", "[unit][benchmark]") {
    // The case the drift figure exists for. These six trials have a wide
    // spread, and the spread alone cannot say whether the machine was noisy or
    // was getting steadily slower. Read in order they are unambiguous.
    const TrialSet slowing = trials_of({10, 10, 20, 20, 30, 30});

    CAPTURE(slowing.drift(), slowing.relative_spread());

    // The second half's median is three times the first's.
    REQUIRE(slowing.drift() > 1.99);
    REQUIRE(slowing.drift() < 2.01);

    // The same six numbers in a different order have the same median, the same
    // quartiles and the same spread, and no drift at all. That is the whole
    // argument for reporting the two separately: sorted, these two sets are
    // indistinguishable.
    const TrialSet noisy = trials_of({10, 20, 30, 10, 20, 30});

    REQUIRE(noisy.median() == slowing.median());
    REQUIRE(noisy.lower_quartile() == slowing.lower_quartile());
    REQUIRE(noisy.upper_quartile() == slowing.upper_quartile());
    REQUIRE(noisy.relative_spread() == slowing.relative_spread());
    REQUIRE(noisy.drift() == 0.0);
}

TEST_CASE("drift is negative when the machine was still warming up", "[unit][benchmark]") {
    // The sign matters, and it caught a real error in this harness: a
    // cool-down long enough to shed heat also let the part drop to an idle
    // clock, so the early trials were the slow ones and every row reported a
    // negative drift. See `Protocol::settling_limit`.
    const TrialSet trials = trials_of({20, 20, 20, 10, 10, 10});

    REQUIRE(trials.drift() < -0.49);
    REQUIRE(trials.drift() > -0.51);
}

TEST_CASE("a rate is quantity over time, and never over nothing", "[unit][benchmark]") {
    REQUIRE(rate_per_second(1000.0, ms(1000)) == 1000.0);
    REQUIRE(rate_per_second(500.0, ms(250)) == 2000.0);

    // A trial that took no measurable time measured nothing, and an infinity in
    // a table of bandwidths is worse than a zero because it looks like a
    // result.
    REQUIRE(rate_per_second(1000.0, Duration{}) == 0.0);
}
