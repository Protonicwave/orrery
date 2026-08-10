#pragma once

/// \file
/// What a set of timings is allowed to claim.
///
/// Section 7 of the implementation plan asks Phase 7 for a benchmark harness
/// with proper statistical treatment: warm-up, repeated trials, median and
/// dispersion rather than best-of, and recorded machine state. This file is the
/// second and third of those.
///
/// ## Why not the best of the run
///
/// Reporting the fastest of n trials is the most common convention in this kind
/// of benchmark and it is the wrong one here. The argument for it is that the
/// fastest trial is the one where the operating system, the other cores and the
/// cache interfered least, so it is closest to the machine's intrinsic speed.
/// That argument holds on a machine with no other tenants and a fixed clock,
/// and this project targets a laptop with an integrated GPU sharing its memory
/// bandwidth and a processor that changes frequency under load. Here the
/// fastest trial is the one taken while the part was still cold, and quoting it
/// is quoting a speed the machine sustains for a fraction of a second.
///
/// The median is what a run of this length actually delivers, and it is robust:
/// a single trial interrupted by something else on the machine moves it by
/// nothing, where a mean would carry the interruption forward.
///
/// ## Why the dispersion is not optional
///
/// A median with no spread beside it cannot be checked. Two medians that differ
/// by five per cent are a result if the trials are reproducible to one per cent
/// and noise if they are reproducible to twenty, and a reader who is not told
/// which cannot tell. Every timing this project reports therefore carries a
/// dispersion, and `docs/performance/` quotes it in the same table as the
/// figure it qualifies.
///
/// The interquartile range is used rather than the standard deviation, for the
/// same reason as the median: one interrupted trial should not be able to
/// change the conclusion. It is reported relative to the median so that
/// measurements of different durations can be compared for reproducibility.
///
/// ## Drift is reported separately from spread
///
/// Thermal throttling does not look like noise. It looks like every trial being
/// slower than the one before it, which inflates the spread while leaving the
/// median plausible, and which a symmetric dispersion measure cannot
/// distinguish from a machine that is merely noisy. So the trials are also
/// compared in the order they were taken, first half against second half, and
/// the difference is reported as its own number. A run whose drift is a few
/// tenths of a per cent was measured at a steady frequency; a run with ten per
/// cent of drift was measuring the cooling system.

#include <cstddef>
#include <vector>

#include "orrery/backend/worker_statistics.hpp"

namespace orrery::benchmark {

/// The clock and duration every measurement here uses.
///
/// Taken from the backend rather than declared again, because Phase 6's
/// per-worker instrumentation already made this choice and a benchmark that
/// timed a region on one clock while the scheduler timed its workers on another
/// could not put the two in the same table. `steady_clock`, for the reason
/// given there: an interval that can go backwards is not a measurement.
using Clock = backend::Clock;
using Duration = backend::Duration;

/// The timings of one configuration, in the order they were taken.
///
/// Construction sorts a copy and keeps the original order, because the two
/// questions this class answers need different views of the same data. How
/// fast was it needs the order statistics; did the machine slow down while we
/// asked needs the sequence.
class TrialSet {
public:
    TrialSet() = default;

    /// Take the trials, in the order they were measured.
    ///
    /// An empty set is permitted and every statistic below is then zero. A
    /// benchmark that measured nothing should print zeros rather than divide by
    /// none of them.
    explicit TrialSet(std::vector<Duration> trials);

    [[nodiscard]] std::size_t count() const noexcept { return ordered_.size(); }

    [[nodiscard]] bool empty() const noexcept { return ordered_.empty(); }

    /// The headline figure, and the one every table quotes.
    [[nodiscard]] Duration median() const noexcept;

    [[nodiscard]] Duration fastest() const noexcept;

    [[nodiscard]] Duration slowest() const noexcept;

    /// The quartiles, which the dispersion is formed from.
    [[nodiscard]] Duration lower_quartile() const noexcept;

    [[nodiscard]] Duration upper_quartile() const noexcept;

    /// The interquartile range as a fraction of the median.
    ///
    /// The number to read first. Under about 0.02 the measurement is
    /// reproducible enough to quote to three figures; above about 0.10 it is
    /// not, and this project says so rather than rounding the doubt away.
    [[nodiscard]] double relative_spread() const noexcept;

    /// How much slower the second half of the run was than the first.
    ///
    /// Positive means the machine got slower while it was being measured, which
    /// on a laptop under a sustained kernel means it got hot. Negative means it
    /// was still warming up, which usually means the warm-up was too short.
    /// Zero is what a steady measurement looks like.
    ///
    /// Compared as medians of the two halves rather than as first trial against
    /// last, so that one interrupted trial at either end cannot manufacture a
    /// drift that was not there. Zero when there are fewer than four trials,
    /// because two halves of one trial each say nothing.
    ///
    /// Computed once, when the set is built. Everything else here is an index
    /// into an already sorted array, but this needs each half sorted on its own,
    /// and doing that on demand would mean an allocation inside a `noexcept`
    /// accessor that a report calls while printing a table.
    [[nodiscard]] double drift() const noexcept { return drift_; }

    /// The trials in the order they were taken.
    ///
    /// For a caller that wants to print the sequence rather than a summary of
    /// it, which is the only way to see a measurement that stepped rather than
    /// drifted.
    [[nodiscard]] const std::vector<Duration>& in_order() const noexcept { return in_order_; }

private:
    /// The trial at a given fraction of the sorted order.
    ///
    /// Nearest rank rather than interpolated. Interpolating between two timings
    /// produces a number no trial produced, which is defensible for a
    /// continuous quantity and misleading for one whose whole interest is
    /// whether the machine did the same thing twice.
    [[nodiscard]] Duration quantile(double fraction) const noexcept;

    std::vector<Duration> in_order_;
    std::vector<Duration> ordered_;
    double drift_{};
};

/// A rate, in whatever unit the caller counted, from a duration.
///
/// Guards the division that a zero-length measurement would otherwise make. A
/// trial that took no measurable time did not measure anything, and reporting
/// an infinite rate for it would put an infinity in a table.
[[nodiscard]] double rate_per_second(double quantity, Duration elapsed) noexcept;

} // namespace orrery::benchmark
