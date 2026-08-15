"""The whole of what crosses between the client and the service.

This module is the contract, and it is the only statement of it. The service
validates and serialises through these models; the browser client reads
`web/src/service/contract.ts`, which is generated from them by
`service/tools/export_contract.py` and checked in continuous integration. A
field added here and not regenerated fails the build rather than reaching a
client that does not know about it.

One source rather than two is the same rule the rest of the project already
follows: the version comes from `CMakeLists.txt`, the design tokens from one
stylesheet, the published gallery from one module. An API is the boundary where
two definitions drift fastest, because the two halves are written in different
languages by people reading different files, and a mismatch shows up as a
number that is quietly undefined rather than as anything that fails.

## What is deliberately not in here

The configuration itself. A submission carries the text of an `.orrery` file
rather than a structure of twenty numbers, for the reason ADR-0051 gives about
the WebAssembly boundary: the browser and the command line then read the same
document with the same reader, and there is no second definition of what a
configuration is to keep in step. This module therefore has one string where a
naive design would have had the whole settings table again.
"""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field

#: What state a job is in.
#:
#: Four rather than five: there is no "cancelled". Nothing can cancel a job,
#: because there are no accounts and so no way to establish that the person
#: asking is the person who submitted it. A run this service accepts is a run it
#: finishes or fails.
JobState = Literal["queued", "running", "done", "failed"]


class Problem(BaseModel):
    """One reason a submission was refused.

    The shape the C++ already reports in: the setting named in the spelling the
    file uses, and what is wrong with it. `orrery run` prints exactly this pair
    for every objection at once, and so does this, for the same reason
    `configuration.hpp` gives: somebody who has written three mistakes should be
    told about three mistakes rather than discover them one submission at a
    time.
    """

    setting: str = Field(description="The setting, as section.key.")
    complaint: str = Field(description="What is wrong with it.")


class Rejection(BaseModel):
    """Everything wrong with a submission, in the order the settings appear."""

    problems: list[Problem]


class Limits(BaseModel):
    """What the service will accept, so the client can say so before submitting.

    Sent rather than compiled into the client, so that a service raising or
    lowering a ceiling does not need the site rebuilt to stop offering runs it
    will refuse. `orrery_service/limits.py` is where the numbers and their
    reasons live.
    """

    max_particles: int
    max_steps: int
    max_work: float = Field(
        description="Particle count times its logarithm times steps, at most."
    )
    max_queue: int
    solvers: list[str] = Field(description="The solver kinds a submission may name.")


class Reference(BaseModel):
    """What this service has actually achieved, for pricing a run.

    Not a figure from `docs/performance.md`. Those were measured on the laptop
    the reports name, and the machine a container lands on is not that laptop,
    so quoting them here would attribute this service's speed to hardware it
    does not have. This is the median over jobs this service has completed, and
    `jobs` says how many, so a client can decline to price anything from one
    sample.

    Absent until something has run. A client with no reference says the estimate
    is not available rather than showing a number nothing measured.
    """

    step_ms: float = Field(description="Median milliseconds a step.")
    particles: int = Field(description="The particle count that median was taken at.")
    jobs: int = Field(description="Completed runs the median is over.")


class Capabilities(BaseModel):
    """What the service can do at the moment it was asked.

    Fetched before the console offers to submit anything. Several conditions
    each independently mean the button should say something other than "submit":
    no worker has beaten recently, the queue is full, the service has spent the
    computing it allows itself in a day, or it did not answer at all. ADR-0054 is
    what the client does in each case.

    Which of them it is arrives as a sentence rather than as flags the client
    turns back into one. The service knows why it is refusing, and a client
    reconstructing the reason from `workers` and `queued` would be a second
    statement of the same rule that quietly stops matching the first the moment a
    new condition is added, which is what happened when the budget was.
    """

    version: str = Field(description="The simulator version the worker runs.")
    limits: Limits
    accepting: bool = Field(
        description=(
            "False when no worker is alive, the queue is full, or the service "
            "has spent its budget for the day."
        )
    )
    refusal: str = Field(
        description=(
            "Empty while accepting. Otherwise one sentence saying which of "
            "those it is, in the service's own words."
        )
    )
    queued: int
    #: Workers whose heartbeat is inside the visibility timeout.
    workers: int
    reference: Reference | None


class Submission(BaseModel):
    """A run somebody wants taken.

    One field. Everything about the run is in the configuration, which is the
    property `configuration.hpp` exists to provide: a configuration file plus a
    revision of this repository determines the trajectory.
    """

    configuration: str = Field(description="The text of an .orrery file.")


class Progress(BaseModel):
    """How far a run has got.

    Every field is measured by the worker from the run's own output rather than
    estimated: the step and the time are read from what the simulator prints,
    the step time is wall clock divided by steps taken since the last report,
    and the energy drift is the last row of the diagnostics file the run is
    writing. A progress bar that interpolates is a progress bar that lies when a
    run slows down.
    """

    step: int
    time: float = Field(description="Model time, in the configuration's units.")
    #: Null until two reports have been seen, since one gives no interval.
    step_ms: float | None
    #: Null until the run has written its first diagnostics row.
    energy_drift: float | None


class Job(BaseModel):
    """A submitted run, at whatever stage it has reached.

    The same shape whether it is read once by polling or pushed repeatedly over
    a WebSocket, so that a client falling back from one to the other changes how
    often it learns things and not what it learns. That is the whole of why the
    socket carries this and not a smaller update message.
    """

    id: str
    state: JobState
    #: When it was submitted, ISO 8601 with an offset.
    created: str
    #: How many are ahead of it, while it is queued. Null once it is running.
    position: int | None
    #: How many times it has been claimed. Above one means a worker died on it.
    attempts: int
    particles: int
    steps: int
    timestep: float
    #: Steps between trajectory frames, so a client knows how many to expect.
    stride: int
    frames: int
    progress: Progress
    #: Empty unless the state is failed, and then a sentence saying how.
    error: str
    #: Where to fetch the result, once there is one. Relative to the API root.
    trajectory: str | None
    diagnostics: str | None


class Accepted(BaseModel):
    """The answer to a submission.

    Carries the whole job rather than only its identifier, because the client's
    next question is always "where am I in the queue" and answering it in the
    same response saves a round trip on the one request a person is watching.

    `duplicate` is true when the configuration had already been submitted and
    this is the earlier job. Jobs are idempotent by the hash of the
    configuration they run, so submitting the same document twice returns the
    first result rather than running it again, and the client says so rather
    than appearing to have queued something that never moves.
    """

    job: Job
    duplicate: bool


class Health(BaseModel):
    """Whether the process is alive, and whether it can do its work.

    Two questions rather than one. A process that is running but cannot reach
    the database should be restarted rather than sent traffic, and an
    orchestrator can only tell those apart if the answers are separate fields.
    """

    alive: bool
    ready: bool
    #: Empty when ready, and otherwise what is not reachable.
    detail: str


#: Every model the client is generated from, in the order they are written out.
#:
#: Listed explicitly rather than discovered by walking the module, so that a
#: model added for the service's internal use does not silently become part of
#: the published contract.
EXPORTED: tuple[type[BaseModel], ...] = (
    Problem,
    Rejection,
    Limits,
    Reference,
    Capabilities,
    Submission,
    Progress,
    Job,
    Accepted,
    Health,
)
