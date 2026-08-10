# ADR-0019: Report the median with its dispersion, and measure the throttling

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Section 7 of the implementation plan asks Phase 7 to establish the measurement
methodology every later performance claim in this project depends on. Section 8
names thermal throttling on a laptop part as a standing risk and requires the
phase to address it directly rather than work around it quietly.

Both matter because of what this project is. Orrery is developed and benchmarked
on one laptop. The processor changes frequency under load within seconds, shares
its memory bandwidth with an integrated GPU on the same die, and has two kinds of
core that differ by a factor of two on the same work. Every figure the project
publishes is taken on that machine, so the question is not whether it can be
measured precisely but what a measurement of it is allowed to claim.

Phase 6 met this in a mild form and said so: its timings had an interquartile
spread from 5 to 36 per cent, and `docs/performance/threading.md` marks them
indicative for that reason. Its conclusions survived because they came from the
per-worker idle-time instrumentation, which is a ratio taken inside one run and
is far more stable than a wall time. Phase 7 has no such luck. Its results *are*
wall times, converted into rates and compared against ceilings.

## Decision

Every timing this project reports is the **median** of repeated trials, quoted
with its **interquartile range** as a fraction of that median, and with a
**drift** figure comparing the second half of the trials against the first. The
harness is `benchmarks/harness/` and no benchmark in this repository times
anything by other means.

Throttling is handled by three mechanisms that do different jobs.

Configurations are separated by a **cool-down**, so a row measured sixth is not
measured at a lower clock than the row measured first.

Warm-up is a **settling loop** rather than a count: trials are discarded until
two consecutive ones agree to within five per cent. This exists because the
cool-down created the opposite error, described below.

The session is bracketed by a **thermal canary**: a fixed block of arithmetic on
one thread, touching no memory, timed at the start and at the end. Nothing but
the clock can change its duration, so the ratio between the two is a direct
measurement of how much the machine slowed down while it was being benchmarked.
It is printed with the results and quoted in `docs/performance/`.

## Alternatives considered

**The best of n trials.** The usual convention, and the one Phase 6 explicitly
declined. The argument for it is that the fastest trial is the one where the
operating system and the other cores interfered least, so it is closest to the
machine's intrinsic speed. That argument assumes the interference is the only
thing varying. On this part the fastest trial is typically the one taken while
the silicon was still cold, and it reports a speed the machine sustains for a
fraction of a second. The harness computes it anyway and prints it in a column
beside the median, so that the size of the difference is visible in this
project's own output rather than only asserted here.

**The mean, with a standard deviation.** More familiar, and wrong for this
distribution. Timings on a shared machine are not symmetric: they have a floor
set by the work and a tail of arbitrary length set by whatever else ran. One
interrupted trial moves the mean and inflates the deviation, where it moves the
median by nothing. The interquartile range is used for the same reason.

**More trials, and treat the noise as random.** Trials are not independent
samples from a fixed distribution, because taking them changes the temperature
of the thing being sampled. Running a hundred of them makes the later ones
systematically slower rather than better characterised, which is exactly the
error the drift figure was added to detect.

**Reporting nothing but the drift-free rows.** Discarding a measurement because
its drift is large would leave the reader with a clean table and no way to know
what was thrown out. Every row is printed with its own spread and drift, and
whether a row is usable is a judgement the reader is given the evidence to make.

**Pinning the clock, or disabling boost.** The right answer on a machine where
it is available. There is no portable interface for it, the platform-specific
ones require administrative rights, and a project whose figures could only be
reproduced by someone who had reconfigured their firmware would fail the
reproducibility standard in section 5 of the implementation plan.

## Consequences

Measurements take longer. A configuration now costs a cool-down, a settling
loop and eleven trials rather than one run, and a full roofline session is
minutes rather than seconds. That is the price of a number that can be checked.

The tables are wider. Every rate carries a spread and a drift, and the reader is
expected to look at them. This is deliberate: a bare number invites a
comparison, and a number beside a twenty per cent spread invites the right one.

The cool-down introduced an error that the drift figure then caught, and the
sequence is worth recording because it is the argument for the whole approach. A
pause long enough to shed heat is also long enough for the part to drop to an
idle frequency, so the first timed trials after it were measuring the clock
ramping back up. Every row of the first session reported a *negative* drift,
meaning each run got faster as it went. That is the opposite of thermal
throttling and could not be anything else, and it produced the settling loop.
Without a drift figure it would have been invisible, and the phase would have
published rates that were ten to thirty per cent low with a plausible-looking
dispersion beside them.

The project can now say what it does not know. Phase 6's timings are marked
indicative; Phase 7's are marked with the conditions they were taken under and
the canary's verdict on the session. A later phase that reports a five per cent
improvement has something to check it against, which is the point of building
this before there are results to defend.
