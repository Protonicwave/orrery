#pragma once

/// \file
/// How a measurement is taken, so that every measurement is taken the same way.
///
/// The statistics in `statistics.hpp` say what a set of timings may claim. This
/// file says how the set is produced, which is the half that decides whether
/// the claim is worth anything.
///
/// ## Warm-up
///
/// The first evaluation of anything on this machine is not like the ones that
/// follow it. It touches every page of its arrays for the first time, so it
/// pays a page fault per four kilobytes; it finds the caches holding somebody
/// else's data; it wakes worker threads that have never run; and on a laptop it
/// runs at a boost frequency the part cannot hold. None of those costs recurs
/// in a simulation that evaluates millions of times, so a benchmark that
/// included them would be measuring the wrong thing. Warm-up trials are run and
/// discarded.
///
/// ## Cooling
///
/// Section 8 of the implementation plan names thermal throttling as a standing
/// risk and requires Phase 7 to address it directly rather than work around it
/// quietly. This is the first of the two things done about it. Between
/// configurations the harness pauses, so that a row measured after five other
/// rows is not measured at a lower clock than the first for reasons that have
/// nothing to do with what it is comparing.
///
/// A pause is a blunt instrument and it is not claimed to be more. It does not
/// return the part to the temperature the first row saw, and no portable
/// interface reports what that temperature is. What it does is make the
/// residual drift small enough to measure, which is the second thing:
/// `TrialSet::drift` reports how much slower the second half of a run was than
/// the first, and `ThermalCanary` reports how much slower the machine got over
/// a whole session. A drifting measurement is then visible in the output rather
/// than baked into it.
///
/// ## The canary
///
/// A fixed arithmetic workload, run on one thread at the start and end of a
/// session and at nothing else. It touches no memory beyond a register, so the
/// only thing that can change its duration is the clock the part is running at.
/// The ratio between the two is a direct measurement of how much the machine
/// slowed down while it was being benchmarked, in a form that does not depend
/// on trusting any of the measurements in between.
///
/// This is the number that decides whether a session's results are usable. A
/// canary that reports two per cent is a session whose figures can be compared
/// with each other; one that reports twenty is a session that measured a laptop
/// cooling system and should be run again on mains power with fewer rows.

#include <chrono>
#include <cstdint>
#include <vector>

#include "harness/statistics.hpp"
#include "orrery/core/function_ref.hpp"

namespace orrery::benchmark {

/// How many times to run something, and how long to wait first.
///
/// A plain aggregate with defaults, so that a benchmark states only the fields
/// it has a reason to change and the rest are the project's convention rather
/// than that program's opinion.
struct Protocol {
    /// Evaluations run and thrown away before anything else.
    ///
    /// Two is enough for the costs that occur exactly once: the page faults on
    /// first touch, and waking threads that have never run.
    int warmup{2};

    /// The most further evaluations to discard while waiting for the clock to
    /// settle, and how close two of them have to be before it counts as
    /// settled.
    ///
    /// This exists because the cool-down below caused a measurable error the
    /// first time this harness was run. A pause long enough to shed heat is
    /// also long enough for the part to drop to an idle frequency, so the first
    /// timed trials after it were measuring the ramp back up. The symptom was
    /// unmistakable and is worth recording: every row reported a *negative*
    /// drift, meaning each run got faster as it went, which is the opposite of
    /// thermal throttling and could not be anything else.
    ///
    /// So warming up is not a count any more. Trials are discarded until two
    /// consecutive ones agree to within `settled_within`, up to
    /// `settling_limit`. The two mechanisms then do different jobs rather than
    /// fighting: the cool-down manages the temperature between configurations,
    /// and this manages the frequency within one.
    ///
    /// Five per cent, because that is comfortably wider than the trial-to-trial
    /// noise on this machine and much narrower than the factor of about 1.4
    /// between an idle clock and a loaded one. The limit is there so that a
    /// genuinely noisy machine produces a measurement with a large spread,
    /// which the reader can see, rather than a program that never returns.
    int settling_limit{24};
    double settled_within{0.05};

    /// Timed evaluations.
    ///
    /// Eleven by default: enough for a median and quartiles to mean something,
    /// odd so that the median is a trial that happened, and few enough that a
    /// table of a dozen rows finishes in minutes rather than an hour. A run
    /// that reports a large spread should be repeated with more rather than
    /// quoted with fewer.
    int trials{11};

    /// How long to wait before a configuration is measured.
    Duration cooldown{std::chrono::milliseconds(750)};
};

/// Run `body` under `protocol` and return the timings.
///
/// Does not cool down first. Cooling is between configurations rather than
/// before each measurement, and the caller knows where its configurations
/// begin.
[[nodiscard]] TrialSet run_trials(const Protocol& protocol, core::FunctionRef<void()> body);

/// Wait for the part to shed some heat.
void cool_down(const Protocol& protocol);

/// The fixed workload described above, and the record of what it measured.
class ThermalCanary {
public:
    /// Time the canary workload and remember it.
    ///
    /// Call once at the start of a session and once at the end. Calling it
    /// between configurations as well is harmless and gives a better picture,
    /// since only the first and last are compared.
    void mark();

    /// How much slower the machine is now than when the canary first ran.
    ///
    /// Positive is slower. Zero before the second mark, because one
    /// measurement is not a comparison.
    [[nodiscard]] double slowdown() const noexcept;

    [[nodiscard]] std::uint64_t marks() const noexcept { return marks_; }

    /// Every timing, in order, so that a report can show whether the machine
    /// slowed steadily or fell off a cliff at one point.
    [[nodiscard]] const TrialSet& history() const noexcept { return history_; }

private:
    std::vector<Duration> timings_;
    TrialSet history_;
    std::uint64_t marks_{0};
};

} // namespace orrery::benchmark
