#include "harness/protocol.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

#include "harness/machine_limits.hpp"
#include "harness/statistics.hpp"
#include "orrery/core/function_ref.hpp"
#include "orrery/core/types.hpp"

namespace orrery::benchmark {

namespace {

/// Time one call of `body` and discard the result.
[[nodiscard]] Duration time_once(core::FunctionRef<void()> body) {
    const Clock::time_point start = Clock::now();
    body();
    return std::chrono::duration_cast<Duration>(Clock::now() - start);
}

/// How much one duration differs from another, as a fraction of the first.
///
/// A body too fast for the clock to resolve gives two zero durations, which is
/// agreement rather than a division by zero. Without that case the settling
/// loop below would run to its limit on anything trivial, which is a test that
/// stalls for no reason and, worse, a short measurement that spends its time
/// settling instead of measuring.
[[nodiscard]] double relative_change(Duration previous, Duration current) {
    if (previous.count() > 0) {
        return std::abs(static_cast<double>((current - previous).count())) /
               static_cast<double>(previous.count());
    }

    return current.count() == 0 ? 0.0 : 1.0;
}

/// Run and discard evaluations until two of them agree.
///
/// See `Protocol::settling_limit` for why this is a loop rather than a count.
/// Returns as soon as the machine is doing the same thing twice, which after a
/// cool-down takes a handful of evaluations and after nothing at all takes one.
void settle(const Protocol& protocol, core::FunctionRef<void()> body) {
    // Two agreements in a row rather than one. A single pair of similar
    // timings happens by chance often enough to end the loop while the clock is
    // still moving, and the cost of asking for three consecutive similar
    // evaluations instead of two is one more discarded evaluation on a machine
    // that was already settled.
    constexpr int kAgreementsRequired = 2;

    Duration previous{};
    bool have_previous = false;
    int agreements = 0;

    for (int attempt = 0; attempt < protocol.settling_limit; ++attempt) {
        const Duration current = time_once(body);

        if (have_previous) {
            const double change = relative_change(previous, current);

            agreements = change < protocol.settled_within ? agreements + 1 : 0;

            if (agreements >= kAgreementsRequired) {
                return;
            }
        }

        previous = current;
        have_previous = true;
    }
}

} // namespace

TrialSet run_trials(const Protocol& protocol, core::FunctionRef<void()> body) {
    for (int warmup = 0; warmup < protocol.warmup; ++warmup) {
        body();
    }

    settle(protocol, body);

    std::vector<Duration> timings;
    timings.reserve(static_cast<std::size_t>(protocol.trials));

    for (int trial = 0; trial < protocol.trials; ++trial) {
        // Reserving the vector above is why the push is inside the timed region
        // and the allocation is not: an allocation inside the loop would land in
        // some trials and not others, and it would land in the early ones,
        // which is exactly where a drift measurement would then find something
        // that was not thermal.
        timings.push_back(time_once(body));
    }

    return TrialSet{std::move(timings)};
}

void cool_down(const Protocol& protocol) {
    std::this_thread::sleep_for(protocol.cooldown);
}

void ThermalCanary::mark() {
    // One block discarded and three timed, of which the median is kept. The
    // discard is for the same reason `settle` exists: the canary is often
    // marked straight after a cool-down, and a first block measured while the
    // core is still at its idle frequency would report the ramp rather than the
    // temperature. The median of three is because a single block can be
    // interrupted, and a canary that reported the machine speeding up under
    // sustained load would be worse than no canary at all.
    constexpr int kBlocksPerMark = 3;

    // Blocks discarded until this much time has passed, so that the timed ones
    // are taken at the frequency the core will hold rather than the one it
    // happened to be at.
    //
    // This is the second thing the canary got wrong and it is worth recording.
    // With a single discarded block, the trace reported the machine getting
    // *faster* through a session of sustained eight-core load, by up to a factor
    // of two. The cause is not thermal at all: a mark taken shortly after the
    // program starts, or after a cool-down, catches a core that the operating
    // system has parked at a low frequency, and seven milliseconds of work is
    // not long enough for the governor to raise it. So the canary was measuring
    // the ramp rather than the ceiling, in the direction that made a throttling
    // machine look like an accelerating one.
    constexpr Duration kSettleFor = std::chrono::milliseconds{60};

    core::Real sink = 0;

    const Clock::time_point settle_until = Clock::now() + kSettleFor;
    while (Clock::now() < settle_until) {
        static_cast<void>(fused_multiply_add_block(kCanaryRounds, &sink));
    }

    std::vector<Duration> blocks;
    blocks.reserve(kBlocksPerMark);

    for (int block = 0; block < kBlocksPerMark; ++block) {
        const Clock::time_point start = Clock::now();

        // The operation count is discarded deliberately. What the canary
        // measures is the time a fixed amount of arithmetic took, and turning
        // that into a rate would invite it to be read as a throughput figure,
        // which it is not: it runs on one thread and makes no attempt to
        // saturate anything.
        static_cast<void>(fused_multiply_add_block(kCanaryRounds, &sink));

        blocks.push_back(std::chrono::duration_cast<Duration>(Clock::now() - start));
    }

    static_cast<void>(sink);

    timings_.push_back(TrialSet{std::move(blocks)}.median());
    history_ = TrialSet{timings_};
    ++marks_;
}

double ThermalCanary::slowdown() const noexcept {
    if (timings_.size() < 2) {
        return 0.0;
    }

    const Duration first = timings_.front();
    const Duration last = timings_.back();

    if (first.count() <= 0) {
        return 0.0;
    }

    return (static_cast<double>(last.count()) / static_cast<double>(first.count())) - 1.0;
}

} // namespace orrery::benchmark
