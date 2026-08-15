"""What a submitted run may ask for.

Every ceiling here is a number this repository can reproduce, and each is
written beside the document that produces it. That constraint is the reason
there is no ceiling expressed in seconds: a second is a property of the machine
the run lands on, and the machine this service runs on is not the laptop
`docs/performance.md` was measured on. What the ceilings bound instead is work,
which is a property of the run, and the service reports its own measured rate
separately so that a client can turn the two into an estimate without anybody
having to invent a figure.

There are two kinds of ceiling here and they answer different questions. The
first bounds one run: how many particles, how many steps, how much work. The
second bounds what a deployment can be made to spend: how often one address may
submit, how much work the service will take on in a day, and how many runs may
be in progress at once. A service with only the first kind is one that a
thousand legal submissions can empty a bank account with.

The ceilings are refusals rather than clamps. A submission asking for more than
one of them comes back with a sentence naming the ceiling and the value that
passed it, because a run quietly cut down to what the service felt like giving
is a run whose result answers a question nobody asked. That is the same
argument `docs/formats/configuration.md` makes about settings the parser might
have skipped, applied one layer out.
"""

from __future__ import annotations

import math
from datetime import timedelta

#: The most particles a submitted run may integrate.
#:
#: Twenty thousand is the largest count this repository has published a measured
#: step time for on the collision, in the demonstration section of README.md.
#: A ceiling above it would be a service accepting runs whose cost nothing here
#: has ever measured.
MAX_PARTICLES = 20_000

#: The most steps a submitted run may take.
#:
#: Independent of the count, because a small run integrated for a long time is a
#: reasonable thing to ask for and is cheap. What stops a large run being taken
#: for a long time is the work ceiling below, not this.
MAX_STEPS = 20_000

#: The fewest particles worth queueing a job for.
#:
#: Two, which is what `problems_with` already requires of a sampled
#: configuration. Stated here as well because this service refuses before it
#: parses in one case, and a message about a ceiling should name the same bound
#: at both ends.
MIN_PARTICLES = 2

#: The most jobs that may be waiting at once.
#:
#: A queue longer than this is one whose last entry will not be reached while
#: anybody is still watching, and telling somebody so when they submit is better
#: than accepting the job and letting them find out. `ADR-0054` is what happens
#: at this boundary.
MAX_QUEUE = 32

#: The largest configuration file the service will read, in bytes.
#:
#: A configuration is about twenty settings and the comment header the examples
#: carry, which is a few hundred bytes. Sixteen kilobytes is generous enough
#: that a heavily commented file is not refused and small enough that nothing is
#: buffered that should not be.
MAX_CONFIGURATION_BYTES = 16 * 1024

#: Which solvers a submission may name.
#:
#: The two SYCL solvers are refused rather than accepted and quietly run on the
#: CPU. `configuration.hpp` explains why the parser accepts them in every build:
#: a configuration file is a document and should mean the same thing whatever
#: reads it. Whether this machine can provide what it asks for is a different
#: question, and this is where that question is answered. The worker has no
#: device, so the honest answer is no.
ALLOWED_SOLVERS = ("direct", "barnes-hut")


def work_units(count: int, steps: int, solver: str) -> float:
    """The work a run of `count` particles over `steps` steps costs.

    Scored on the curve the solver it names actually follows: the square of the
    count for the direct solver, which computes every pair, and the count times
    its logarithm for the tree, which is what `docs/performance/barnes_hut.md`
    measures. One score for both would have to be wrong about one of them, and
    being wrong about the direct solver is the expensive direction: at the
    particle ceiling it is several thousand times the pair work.

    The two are not in the same units and are not claimed to be. A tree
    interaction is preceded by a walk and a direct one is not, so the same score
    is a slightly different number of seconds on each. What the ceiling needs is
    a bound rather than an estimate, and comparing both against the tree's
    reference errs towards refusing the direct run, which is the right way round.
    """
    if count < 2:
        return 0.0
    per_step = count * count if solver == "direct" else count * math.log2(count)
    return per_step * steps


#: The most work one submission may ask for.
#:
#: The demonstration in README.md: twenty thousand particles over six thousand
#: steps, which took 122 seconds on the machine `docs/performance.md` names.
#: That run is the largest this repository states a wall clock for, so it is the
#: largest thing the service has any measured basis for accepting.
#:
#: The two ceilings above interact with this one rather than duplicating it. A
#: run at the particle ceiling gets six thousand steps; a run of two thousand
#: particles gets the full twenty thousand. Both are the same amount of work.
MAX_WORK = work_units(MAX_PARTICLES, 6_000, "barnes-hut")

#: How many runs one address may submit in `WINDOW`.
#:
#: Six. A run the size of the demonstration took 122 seconds on the machine
#: `docs/performance.md` names, so six of them is more than an hour of a
#: worker's time and rather more than one person can watch. It is also loose
#: enough for the loop the editor is for: design a collision, submit it, change
#: an angle, submit it again.
#:
#: Counted over submissions that queued a run rather than over requests. A
#: configuration that has already been submitted returns the first job without
#: running anything, and charging somebody for asking a question twice would be
#: a ceiling on curiosity rather than on cost.
MAX_SUBMISSIONS_PER_ADDRESS = 6

#: The window the count above and the budget below are measured over.
WINDOW = timedelta(hours=1)

#: The most work the whole service will take on in `BUDGET_WINDOW`.
#:
#: Twenty-four times one full-size run, which on the only wall clock this
#: repository has measured is about fifty minutes of computing a day. That is the
#: figure to change when the bill is wrong: everything else here bounds one
#: submission, and this is the only ceiling on what a day of strangers can cost.
#:
#: Scored in the same work units a submission is checked against, because the
#: alternative is seconds, and a second is a property of the machine a container
#: landed on rather than of the runs it was asked for.
BUDGET = 24 * MAX_WORK

#: The window the budget is measured over.
#:
#: Rolling rather than a calendar day. A budget that resets at midnight is one
#: that is spent in the first hour after midnight, and a rolling window refuses
#: the twenty-fifth run of the last day rather than the first run of this one.
BUDGET_WINDOW = timedelta(hours=24)

#: How many runs may be in progress at once, across every worker.
#:
#: Two. The deployment runs one worker and one run at a time, and the second is
#: there so that a rollout with an old worker and a new one for a minute is not a
#: deployment that stops taking runs. A cap above the number of workers a
#: deployment actually has would not be a cap.
MAX_RUNNING = 2

#: How long a finished run's trajectory is kept.
#:
#: Seven days. A submitted run is watched while it happens and shared for a day
#: or two afterwards, and a trajectory is tens of megabytes, so keeping every one
#: for ever is a bill that grows with the number of visitors and a bucket nobody
#: reads. The published gallery is not affected: those runs are static assets
#: built into the site rather than jobs in this store.
RETENTION = timedelta(days=7)

#: The largest request body the API will read, in bytes.
#:
#: Four times the configuration ceiling, so that a legal configuration cannot be
#: refused here for the JSON envelope and the escaping around it, and a body that
#: is not a configuration is dropped before anything parses it.
#: `MAX_CONFIGURATION_BYTES` is the ceiling with the message; this is the one
#: that stops a megabyte being read into memory in order to be told so.
MAX_BODY_BYTES = 4 * MAX_CONFIGURATION_BYTES
