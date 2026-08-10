#include "harness/statistics.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace orrery::benchmark {

namespace {

/// The median of an already sorted range, by nearest rank.
///
/// The upper of the two middle trials at even counts rather than their mean,
/// for the reason `quantile` gives: every figure this class reports is a trial
/// that happened.
[[nodiscard]] Duration middle_of(const std::vector<Duration>& sorted) noexcept {
    return sorted.empty() ? Duration{} : sorted[sorted.size() / 2];
}

/// How much slower the second half of a sequence was than the first.
///
/// Split from the constructor only for length. Each half is sorted on its own
/// so that the comparison is between two medians rather than between two
/// arbitrary orderings; the halves themselves are taken from the sequence as
/// measured, which is the whole point, since a sorted copy has thrown that
/// information away.
[[nodiscard]] double drift_of(const std::vector<Duration>& in_order) {
    // Four trials is the fewest that gives two halves of two, which is the
    // fewest from which a median of a half means anything at all.
    constexpr std::size_t kMinimumTrials = 4;

    if (in_order.size() < kMinimumTrials) {
        return 0.0;
    }

    const auto half = static_cast<std::ptrdiff_t>(in_order.size() / 2);

    std::vector<Duration> first(in_order.begin(), in_order.begin() + half);
    std::vector<Duration> second(in_order.end() - half, in_order.end());

    std::ranges::sort(first);
    std::ranges::sort(second);

    const Duration early = middle_of(first);
    const Duration late = middle_of(second);

    if (early.count() <= 0) {
        return 0.0;
    }

    return (static_cast<double>(late.count()) / static_cast<double>(early.count())) - 1.0;
}

} // namespace

TrialSet::TrialSet(std::vector<Duration> trials)
    : in_order_(std::move(trials)), ordered_(in_order_), drift_(drift_of(in_order_)) {
    std::ranges::sort(ordered_);
}

Duration TrialSet::median() const noexcept {
    return quantile(0.5);
}

Duration TrialSet::fastest() const noexcept {
    return ordered_.empty() ? Duration{} : ordered_.front();
}

Duration TrialSet::slowest() const noexcept {
    return ordered_.empty() ? Duration{} : ordered_.back();
}

Duration TrialSet::lower_quartile() const noexcept {
    return quantile(0.25);
}

Duration TrialSet::upper_quartile() const noexcept {
    return quantile(0.75);
}

Duration TrialSet::quantile(double fraction) const noexcept {
    if (ordered_.empty()) {
        return Duration{};
    }

    const auto position = static_cast<std::size_t>(fraction * static_cast<double>(ordered_.size()));

    // Clamped rather than trusted: a fraction of one would index one past the
    // end, and the caller asking for the top of the distribution means the
    // largest trial rather than a fault.
    return ordered_[std::min(position, ordered_.size() - 1)];
}

double TrialSet::relative_spread() const noexcept {
    const Duration centre = median();
    if (centre.count() <= 0) {
        return 0.0;
    }

    const auto range = static_cast<double>((upper_quartile() - lower_quartile()).count());
    return range / static_cast<double>(centre.count());
}

double rate_per_second(double quantity, Duration elapsed) noexcept {
    if (elapsed.count() <= 0) {
        return 0.0;
    }

    constexpr double kNanosecondsPerSecond = 1e9;
    return quantity * kNanosecondsPerSecond / static_cast<double>(elapsed.count());
}

} // namespace orrery::benchmark
