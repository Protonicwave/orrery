# ADR-0016: Balance the threads dynamically by stealing ranges

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Section 2 of the implementation plan describes the target machine as four Lion
Cove performance cores beside four Skymont efficiency cores. Phase 6 measured
what that asymmetry is worth: the same direct summation kernel, on the same
configuration, takes 207 ms pinned to a performance core and 448 ms pinned to an
efficiency core. The performance cores are 2.17 times the throughput of the
efficiency cores on this work.

That number decides how the force loop must be divided. Dividing a range into
eight equal shares is what almost every parallel loop does, and on a machine
whose cores are alike it is very nearly optimal with no scheduling cost at all.
Here it means the four fast cores finish their share in a little under half the
time the four slow ones need, and then wait. The region cannot end until the
last worker finishes, so the waiting is throughput the machine had and did not
use.

The alternative costs something. Any scheme that hands work out during the
region needs synchronisation on every hand-out, and that synchronisation is on
the path of the innermost parallel loop of the whole project.

## Decision

The range of target particles is divided into equal shares as before, and a
worker that exhausts its own share takes work from the back of another worker's.
Each share is a half-open range of indices guarded by a mutex, claimed in chunks
of roughly a sixteenth of a share; the owner claims from the front and a thief
from the back. This is `WorkStealingExecutor`, and it is what the project uses.

`StaticExecutor` is kept, correct and tested, as the arm the comparison is made
against.

## Alternatives considered

**Equal fixed shares.** Measured rather than assumed, which is the reason
`StaticExecutor` exists. With the workers pinned so that idle time can be
attributed to a class of core, static partitioning leaves the performance cores
idle for 64.5 per cent of the region while the efficiency cores idle for 15.1
per cent. Stealing brings those to 8.8 and 6.6 per cent, and the wall time from
71.7 ms to 41.7 ms. The full table is in `docs/performance/threading.md`.

**Weighted static shares.** Give the performance cores a larger fixed share in
the ratio the hardware suggests, and keep the zero scheduling overhead. This is
the most tempting alternative because the 2.17 above looks like the weight to
use, and it is rejected because that number is not a constant. It moves with the
thermal state of a laptop that throttles within seconds of sustained load, with
whether the machine is on mains or battery, with what else is running, with the
integrated GPU's share of the memory bandwidth, and with the arithmetic mix of
the kernel being scheduled. A weight is a measurement baked in at the wrong
time. Stealing measures the same ratio continuously and for free: under the
work-stealing scheme the performance cores took 15,360 to 16,256 particles each
and the efficiency cores 6,592 to 6,784, a ratio of 2.38 that nothing in the
code was told.

**OpenMP dynamic or guided scheduling.** A well-tested implementation of this
idea that would have taken one directive. It is rejected on dependency grounds
rather than technical ones: OpenMP support differs across the three compilers
this project must build with, it is awkward on macOS with Apple's toolchain, and
it would put a second parallelism model beside the SYCL runtime that Phase 9
introduces. The scheduler here is about two hundred lines and is instrumented in
a way an OpenMP runtime is not, which matters because per-worker idle time is a
deliverable of this phase rather than a debugging aid.

**A lock-free Chase-Lev deque.** The textbook work-stealing structure, and the
right one for a scheduler supporting nested spawning and arbitrary task graphs.
This scheduler supports neither: every parallel region is a flat loop whose
extent is known before it starts, which is a much weaker problem than the one
the deque solves. Against that, the lock-free protocol for a range claimable
from both ends has a genuinely subtle race where the two ends meet, and its
correctness argument lives in a paper rather than in the file.

The mutex is not on a hot path. A chunk is a sixteenth of a worker's share, so
the lock is taken a few tens of times per worker per region while each chunk is
tens of microseconds of arithmetic, and it is uncontended almost always because
a thief only reaches a victim's mutex after exhausting its own work. Section 5
of the implementation plan asks that any single file be defensible to a reviewer
who did not write it, and a lock whose cost is under a thousandth of the region
is what that costs here. If Phase 8's tree walk turns out to need finer chunks,
this is the decision to revisit, and it should be revisited with a measurement.

**Pinning the workers to cores.** Tried, measured, and not adopted as a default.
Pinning makes the static scheme markedly worse rather than better, 71.7 ms
against 47.2 ms unpinned, because it removes the operating system's ability to
migrate a thread off a core that has finished. Both Windows and Linux know about
hybrid topologies and place threads with information a fixed assignment does not
have. Pinning remains available as a measurement instrument, because attributing
idle time to a class of core requires that a worker stay on one, and every
per-class figure this project quotes comes from a pinned run for that reason.

## Consequences

The project carries a scheduler it has to maintain, and a second executor kept
alive only to lose a comparison. Both are the price of the claim being
falsifiable rather than asserted.

Work stealing costs something when it is not needed. On a machine whose cores
are alike, the chunk hand-outs and the steal attempts are pure overhead against
static partitioning, and `StaticExecutor` should win there. Nothing in this
decision claims otherwise; it claims that on the machine described in section 2
of the plan, the overhead is repaid many times over.

The kernel keeps a property that would otherwise have been lost. Because the
division is over target particles and each target reads every source and writes
only itself, a threaded evaluation is bit for bit identical to a serial one,
whatever the thread count or the order chunks were claimed in. ADR-0015 chose
that form in Phase 5 partly in anticipation of this, and
`tests/solvers/parallel_direct_solver_test.cpp` asserts it for equality rather
than against a tolerance. A future scheduler that split the inner sum across
threads would break it, and would then need this ADR superseded and the
direct solver's standing as the project's reference re-argued.

Chunk size is now a constant that affects performance. It is set at sixteen
chunks per worker, and anything from about eight to sixty-four behaves the same
on this machine, which is the sign of a value in the flat part of the curve
rather than one that has been tuned onto a peak.
