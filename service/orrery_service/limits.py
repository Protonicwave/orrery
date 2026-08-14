"""What a submitted run may ask for.

Every ceiling here is a number this repository can reproduce, and each is
written beside the document that produces it. That constraint is the reason
there is no ceiling expressed in seconds: a second is a property of the machine
the run lands on, and the machine this service runs on is not the laptop
`docs/performance.md` was measured on. What the ceilings bound instead is work,
which is a property of the run, and the service reports its own measured rate
separately so that a client can turn the two into an estimate without anybody
having to invent a figure.

The ceilings are refusals rather than clamps. A submission asking for more than
one of them comes back with a sentence naming the ceiling and the value that
passed it, because a run quietly cut down to what the service felt like giving
is a run whose result answers a question nobody asked. That is the same
argument `docs/formats/configuration.md` makes about settings the parser might
have skipped, applied one layer out.
"""

from __future__ import annotations

import math

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


def work_units(count: int, steps: int) -> float:
    """The work a run of `count` particles over `steps` steps costs.

    Particle count times its logarithm times the steps, which is how the tree
    solver scales and what `docs/performance/barnes_hut.md` measures. It is the
    right shape for the ceiling even for a submission naming the direct solver,
    where the true cost goes as the square: a ceiling that let a direct run
    through because it was scored on the tree's curve would be a ceiling that
    understated the expensive case, so the direct solver is bounded further by
    the particle ceiling being the same one the tree gets.
    """
    if count < 2:
        return 0.0
    return count * math.log2(count) * steps


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
MAX_WORK = work_units(MAX_PARTICLES, 6_000)
