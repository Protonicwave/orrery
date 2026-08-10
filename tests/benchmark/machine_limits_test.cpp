#include "harness/machine_limits.hpp"

#include <chrono>
#include <cstddef>
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/protocol.hpp"
#include "orrery/backend/serial_executor.hpp"
#include "orrery/core/types.hpp"

/// \file
/// That the ceilings are measurements of something.
///
/// A probe cannot be tested against a known answer, because what it measures is
/// the machine it is running on and there is no expected value. What it can be
/// tested for is the failure that would matter: a probe whose loop the
/// optimiser deleted reports a rate of many terabytes per second and looks like
/// a triumph. Every case below is a way of catching that, or of catching the
/// opposite error of a probe that did nothing at all.
///
/// The buffers here are small and the trial counts low. These are correctness
/// tests running inside the ordinary suite, not measurements; the measurements
/// come from `benchmarks/roofline.cpp` on a quiet machine.

namespace {

using orrery::backend::SerialExecutor;
using orrery::benchmark::fused_multiply_add_block;
using orrery::benchmark::kCanaryRounds;
using orrery::benchmark::Limit;
using orrery::benchmark::measure_divide_and_sqrt_throughput;
using orrery::benchmark::measure_peak_throughput;
using orrery::benchmark::measure_read_bandwidth;
using orrery::benchmark::measure_triad_bandwidth;
using orrery::benchmark::Protocol;
using orrery::core::Real;

/// Small and quick, because these are tests. One megabyte still exceeds the
/// level-one cache of either kind of core, so the loop is a streaming loop
/// rather than a resident one, which is the shape being exercised.
constexpr std::size_t kTestBytes = std::size_t{1} << 20U;

[[nodiscard]] Protocol quick() {
    return Protocol{
        .warmup = 1, .settling_limit = 2, .trials = 3, .cooldown = std::chrono::milliseconds{0}};
}

/// Every probe has to satisfy this, and a deleted loop satisfies none of it.
void check(const Limit& limit) {
    CAPTURE(limit.name, limit.unit, limit.quantity_per_trial, limit.rate());

    REQUIRE_FALSE(limit.name.empty());
    REQUIRE_FALSE(limit.unit.empty());

    // Work was accounted for.
    REQUIRE(limit.quantity_per_trial > 0);

    // Time passed. A probe whose loop was optimised away returns immediately,
    // and on a clock with nanosecond resolution that shows up as a zero or
    // near-zero median, which is the thing to catch.
    REQUIRE(limit.trials.median().count() > 0);
    REQUIRE(limit.trials.count() == 3);

    // A rate follows from the two above, and the fastest trial cannot be slower
    // than the median, so the best rate cannot be below the sustained one.
    REQUIRE(limit.rate() > 0);
    REQUIRE(limit.best_rate() >= limit.rate());
}

} // namespace

TEST_CASE("the read bandwidth probe accounts for every byte it read", "[unit][benchmark]") {
    SerialExecutor executor;
    const Limit limit = measure_read_bandwidth(executor, quick(), kTestBytes);

    check(limit);

    // One byte counted per byte read. The buffer is sized in elements, so the
    // count is the byte budget rounded down to a whole number of them.
    REQUIRE(limit.quantity_per_trial == static_cast<double>(kTestBytes));
}

TEST_CASE("the triad probe counts two reads and a write", "[unit][benchmark]") {
    SerialExecutor executor;
    const Limit limit = measure_triad_bandwidth(executor, quick(), kTestBytes);

    check(limit);

    // Three buffers sharing the budget, three bytes counted per element per
    // scalar. The convention is stated in the header and asserted here so that
    // a change to it is a test failure rather than a silently different number
    // in a published table.
    const std::size_t elements = kTestBytes / 3 / sizeof(Real);
    REQUIRE(limit.quantity_per_trial == 3.0 * static_cast<double>(elements * sizeof(Real)));
}

TEST_CASE("both arithmetic probes do the work they claim", "[unit][benchmark]") {
    SerialExecutor executor;

    const Limit multiply_add = measure_peak_throughput(executor, quick());
    const Limit divide_and_sqrt = measure_divide_and_sqrt_throughput(executor, quick());

    check(multiply_add);
    check(divide_and_sqrt);

    REQUIRE(multiply_add.unit == "flop");
    REQUIRE(divide_and_sqrt.unit == "op");

    // The finding this project's roofline turns on: a square root and a
    // division are enormously slower than a multiply-add on this kind of
    // hardware. Asserted loosely, as a factor of two rather than the order of
    // magnitude actually measured, because it has to hold on every machine the
    // suite runs on and not only on the target part.
    CAPTURE(multiply_add.rate(), divide_and_sqrt.rate());
    REQUIRE(divide_and_sqrt.rate() * 2.0 < multiply_add.rate());
}

TEST_CASE("the canary block performs and returns the arithmetic", "[unit][benchmark]") {
    Real sink = 0;
    const double operations = fused_multiply_add_block(kCanaryRounds, &sink);

    CAPTURE(operations, sink);

    REQUIRE(operations > 0);

    // Every chain converges to one from a distinct starting value, so the sum
    // over the chains and lanes is positive and finite. A zero would mean the
    // loop never ran, and a NaN or an infinity would mean the recurrence
    // diverged, which is the failure the choice of constants exists to avoid.
    REQUIRE(sink > 0);
    REQUIRE(sink < static_cast<Real>(1e6));
}
