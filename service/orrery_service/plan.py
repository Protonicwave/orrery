"""What an accepted submission becomes: a run, with its output decided.

A submission says what to integrate. It does not say what to write down, where
to write it or how often, and this is where those are settled. Keeping the
decision here rather than in the worker means the answer is known when the job
is stored, so the client can be told how many frames to expect before anything
has started running.

The strides are chosen the way the published gallery's are chosen, and for the
same reasons `web/src/gallery/runs.ts` gives: about four hundred frames, which
is roughly seven seconds of playback, and about a hundred diagnostics samples,
which is what the data rail plots. Both are downloads rather than physics, and
neither changes the trajectory: the run takes the steps it was asked for and
these decide which of them are recorded.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, replace

from .configuration import Configuration, OutputSettings, write_configuration
from .validation import particle_count

#: About how many frames a submitted run's trajectory should hold.
FRAMES = 400

#: About how many diagnostics samples it should hold.
#:
#: Measuring the conserved quantities costs an N-squared pass that shares no
#: code with the solver, which is what makes it a check rather than a readout,
#: so it is taken a hundred times over a run rather than every step.
SAMPLES = 100

#: What the worker's two output files are called, in its own working directory.
#:
#: Fixed rather than derived from the job's identifier. The worker runs each job
#: in a directory of its own, so there is nothing for a name to collide with,
#: and a fixed name means the command the worker runs can be read here.
TRAJECTORY_FILE = "trajectory.otj"
DIAGNOSTICS_FILE = "diagnostics.csv"


@dataclass(frozen=True)
class Plan:
    """An accepted run, with everything about it settled."""

    #: The configuration as the worker will write it, output section included.
    configuration: Configuration
    #: That configuration as text, which is what the worker hands the binary
    #: and what the content hash is taken over.
    text: str
    #: Hexadecimal SHA-256 of `text`.
    content_hash: str
    particles: int
    steps: int
    stride: int
    frames: int
    diagnostics_stride: int
    samples: int


def _stride(steps: int, wanted: int) -> int:
    """The stride that records about `wanted` samples over `steps` steps.

    At least one, since a stride of zero means the two ends of the run and
    nothing between them, which is not what a run being watched wants.
    """
    return max(1, steps // wanted)


def plan(configuration: Configuration) -> Plan:
    """The run `configuration` becomes once the service has decided its output.

    The configuration is validated before this is called. Nothing here checks
    anything: it is the settling of what was left open, and a function that both
    decided and objected would be one whose caller could not tell which it had
    done.
    """
    steps = configuration.run.steps
    stride = _stride(steps, FRAMES)
    diagnostics_stride = _stride(steps, SAMPLES)

    settled = replace(
        configuration,
        output=OutputSettings(
            trajectory_path=TRAJECTORY_FILE,
            trajectory_stride=stride,
            # Positions and masses, as the published runs are written. A reader
            # needs no more to draw one, and the file is twice the size with
            # velocities in it.
            trajectory_velocities=False,
            diagnostics_path=DIAGNOSTICS_FILE,
            diagnostics_stride=diagnostics_stride,
            # No checkpoints. A submitted run is never resumed: the queue
            # retries a job by taking it again from the start, which is the
            # simpler thing to make correct and costs a few minutes at this
            # size.
            checkpoint_path="",
            checkpoint_stride=0,
        ),
    )

    text = write_configuration(settled)

    return Plan(
        configuration=settled,
        text=text,
        # Over the settled text rather than over what was submitted, so that two
        # files differing only in their comments, their whitespace or the order
        # of their sections are one job. That is the property idempotency wants:
        # the same run submitted twice, however it was spelled, is run once.
        content_hash=hashlib.sha256(text.encode("utf-8")).hexdigest(),
        particles=particle_count(settled),
        steps=steps,
        stride=stride,
        # The last step of a run is always written whatever the stride, and the
        # first is written before any step is taken, so the count is the whole
        # divisions plus one. `docs/formats/trajectory.md` is the specification.
        frames=steps // stride + 1,
        diagnostics_stride=diagnostics_stride,
        samples=steps // diagnostics_stride + 1,
    )
