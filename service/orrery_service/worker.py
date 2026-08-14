"""The process that actually runs simulations.

It claims a job, writes the configuration to a file, runs the native `orrery`
binary over it, watches what that prints, uploads what it wrote, and records
what happened. Nothing else. The physics is the binary's, the decisions about
what may be run were made before the job was stored, and this is the part that
turns a row into a trajectory.

Four things here are worth reading rather than skimming.

**The configuration is a file, never an argument.** The worker writes it and
passes a path. Nothing a submitter sent is ever interpolated into a command
line, and there is no shell between this and the binary: `Popen` is given an
argument vector. A configuration is data, and the one way to keep it data is
never to let it be anything else.

**The heartbeat is what holds the claim.** While the run is going, this says so
every few seconds and reports how far it has got. If a beat comes back refused,
the job has been given to somebody else: the run is killed and nothing is
uploaded, because two workers writing two trajectories for one row would end
with the loser's result overwriting the winner's.

**The result is uploaded before the job is finished.** In that order, so that a
job which says it is done is a job whose trajectory is there to be fetched. The
other order has a window in which the client is told to fetch something that has
not arrived, and that window is exactly as long as an upload.

**The simulator is run synchronously, in a thread.** `asyncio` has a subprocess
API and it is not used, because on Windows it needs the proactor event loop and
psycopg's connections need the selector one, and a worker needs both. A thread
running `Popen` needs neither and works everywhere. Nothing is lost: this
process runs one simulation at a time, so there is no concurrency here for an
async subprocess to provide.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import re
import signal
import socket
import subprocess
import tempfile
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

from .database import pool, use_compatible_event_loop
from .plan import DIAGNOSTICS_FILE, TRAJECTORY_FILE
from .queue import Claim, Jobs
from .settings import Settings
from .storage import Storage, StorageError, diagnostics_key, trajectory_key

logger = logging.getLogger(__name__)

#: What the simulator prints as it goes: `  step 1200, time 1.2`.
#:
#: `apps/orrery.cpp` prints twenty of these over a run, which is one every five
#: per cent. That is coarse for a progress bar and it is what the run actually
#: reports, so it is what the client is told: the alternative is a bar that
#: interpolates and therefore lies whenever a run slows down.
_PROGRESS = re.compile(r"^\s*step (\d+), time ([-+0-9.eE]+)\s*$")

#: How long to wait before asking for work again when there is none.
IDLE_SECONDS = 2.0

#: How many lines of the run's output are kept for a failure message.
#:
#: The last hundred. A run that failed says why in its final few lines, and a
#: message long enough to need scrolling is one nobody reads to the end of.
TAIL_LINES = 100

#: How much of that is put in the message.
ERROR_CHARACTERS = 2000


@dataclass
class Run:
    """A simulation in progress, as the heartbeat sees it.

    Written by the thread running the simulator and read by the heartbeat.
    Every field is a number replaced whole, so the two need no lock between
    them: the heartbeat reads whatever the last complete write left, which is
    exactly what a progress report wants.
    """

    process: subprocess.Popen[str] | None = None
    step: int = 0
    time: float = 0.0
    step_ms: float | None = None
    energy_drift: float | None = None
    tail: deque[str] = field(default_factory=lambda: deque(maxlen=TAIL_LINES))

    def output(self) -> str:
        return "\n".join(self.tail)[-ERROR_CHARACTERS:]


def worker_name() -> str:
    """What this worker calls itself.

    The host and the process, which is enough to tell two apart inside one
    deployment and enough to find one in a log. Not a random identifier: a
    worker that restarts should be recognisable as the same machine.
    """
    return f"{socket.gethostname()}-{os.getpid()}"


def last_energy_drift(path: Path) -> float | None:
    """The relative energy error from the last row the run has written.

    Read from the file the run is writing rather than computed here. The column
    is the one `sim/diagnostics_log.cpp` writes as `relative_energy_error`, and
    it is found by name rather than by position, so that a column inserted
    before it is noticed here rather than reported as a drift of whatever
    happened to be in that field.
    """
    try:
        with path.open("r", encoding="utf-8") as file:
            header = file.readline().strip().split(",")
            if "relative_energy_error" not in header:
                return None
            column = header.index("relative_energy_error")
            last = None
            for line in file:
                if line.strip():
                    last = line
    except OSError:
        return None

    if last is None:
        return None
    fields = last.strip().split(",")
    if column >= len(fields):
        return None
    try:
        return float(fields[column])
    except ValueError:
        return None


def binary_version(binary: str) -> str:
    """What the simulator answers to `--version`.

    Reported to the queue so that the API can say which simulator produced a
    trajectory. A worker whose binary will not run says so at start-up rather
    than on its first job.
    """
    result = subprocess.run(
        [binary, "--version"],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"{binary} --version exited {result.returncode}: {result.stderr.strip()}"
        )
    return result.stdout.strip()


class Worker:
    """One worker: claim, run, upload, record, repeat."""

    def __init__(self, settings: Settings, jobs: Jobs, storage: Storage) -> None:
        self._settings = settings
        self._jobs = jobs
        self._storage = storage
        self._name = worker_name()
        self._version = ""
        self._stopping = asyncio.Event()

    @property
    def name(self) -> str:
        return self._name

    def stop(self) -> None:
        """Finish the run in progress and then come down.

        A run is minutes of work that would have to be done again, so a signal
        stops the worker taking anything new rather than abandoning what it
        holds. A deployment that cannot wait kills the process, and the job goes
        back to the queue when the heartbeat stops, which is the same path a
        machine dying takes.
        """
        self._stopping.set()

    async def run_forever(self) -> None:
        self._version = await asyncio.to_thread(binary_version, self._settings.binary)
        logger.info("worker %s running %s", self._name, self._version)

        while not self._stopping.is_set():
            await self._jobs.announce(self._name, self._version)
            claim = await self._jobs.claim(
                self._name, max_attempts=self._settings.max_attempts
            )
            if claim is None:
                # Waiting on the stop event rather than sleeping, so a signal
                # while the queue is empty is answered at once.
                with contextlib.suppress(TimeoutError):
                    await asyncio.wait_for(self._stopping.wait(), IDLE_SECONDS)
                continue

            logger.info("claimed %s, attempt %d", claim.id, claim.attempts)
            try:
                await self._take(claim)
            except Exception as error:
                logger.exception("job %s failed in the worker", claim.id)
                await self._jobs.fail(
                    claim.id, self._name, f"the worker failed: {error}"
                )

    async def _take(self, claim: Claim) -> None:
        with tempfile.TemporaryDirectory(prefix="orrery-") as directory:
            workspace = Path(directory)
            configuration = workspace / "run.orrery"
            configuration.write_text(
                claim.configuration, encoding="utf-8", newline="\n"
            )

            run = Run()
            # Set by the heartbeat when the claim is refused, which is how the
            # code below learns that the job now belongs to another worker.
            lost = asyncio.Event()

            beating = asyncio.create_task(self._beat(claim, run, workspace, lost))
            started = time.monotonic()
            try:
                code = await asyncio.to_thread(
                    self._execute, configuration, workspace, run
                )
            finally:
                beating.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await beating
            elapsed = time.monotonic() - started

            if lost.is_set() or not await self._jobs.beat(claim.id, self._name):
                logger.warning(
                    "job %s was taken from this worker; dropping it", claim.id
                )
                return

            if code != 0:
                await self._jobs.fail(
                    claim.id, self._name, f"the run exited {code}: {run.output()}"
                )
                return

            await self._deliver(claim, workspace, elapsed)

    def _execute(self, configuration: Path, workspace: Path, run: Run) -> int:
        """Run the simulator, following what it prints. Blocking, in a thread.

        The step time is measured between two progress reports rather than
        averaged from the start, so that a run which slows down says so while it
        is happening rather than at the end.
        """
        process = subprocess.Popen(
            [self._settings.binary, "run", str(configuration)],
            cwd=workspace,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        run.process = process

        previous_step = 0
        previous_at = time.monotonic()

        assert process.stdout is not None
        for raw in process.stdout:
            line = raw.rstrip("\r\n")
            run.tail.append(line)

            found = _PROGRESS.match(line)
            if found is None:
                continue

            step = int(found.group(1))
            now = time.monotonic()
            if step > previous_step:
                run.step_ms = (now - previous_at) * 1000 / (step - previous_step)
            run.step = step
            run.time = float(found.group(2))
            previous_step, previous_at = step, now

        return process.wait()

    async def _beat(
        self, claim: Claim, run: Run, workspace: Path, lost: asyncio.Event
    ) -> None:
        """Hold the claim, and report progress, until the run ends or is lost."""
        diagnostics = workspace / DIAGNOSTICS_FILE
        while True:
            await asyncio.sleep(self._settings.heartbeat_interval.total_seconds())
            run.energy_drift = await asyncio.to_thread(last_energy_drift, diagnostics)
            held = await self._jobs.beat(
                claim.id,
                self._name,
                step=run.step,
                time=run.time,
                step_ms=run.step_ms,
                energy_drift=run.energy_drift,
            )
            if held:
                continue

            # The job is somebody else's now. Kill the run rather than letting
            # it finish: what it would produce is a second trajectory for one
            # row, and the later upload would replace the winner's.
            logger.warning("lost the claim on %s; stopping the run", claim.id)
            lost.set()
            if run.process is not None:
                with contextlib.suppress(ProcessLookupError, OSError):
                    run.process.kill()
            return

    async def _deliver(self, claim: Claim, workspace: Path, elapsed: float) -> None:
        """Upload what the run wrote, then record that the job is done."""
        trajectory = workspace / TRAJECTORY_FILE
        diagnostics = workspace / DIAGNOSTICS_FILE
        if not trajectory.exists():
            await self._jobs.fail(
                claim.id, self._name, "the run finished without writing a trajectory"
            )
            return

        try:
            stored = await asyncio.to_thread(
                self._storage.put_file,
                trajectory_key(claim.id),
                trajectory,
                "application/octet-stream",
            )
            await asyncio.to_thread(
                self._storage.put_file,
                diagnostics_key(claim.id),
                diagnostics,
                "text/csv",
            )
        except StorageError as error:
            await self._jobs.fail(
                claim.id, self._name, f"the result could not be stored: {error}"
            )
            return

        drift = await asyncio.to_thread(last_energy_drift, diagnostics)
        await self._jobs.finish(
            claim.id,
            self._name,
            trajectory_key=trajectory_key(claim.id),
            diagnostics_key=diagnostics_key(claim.id),
            trajectory_bytes=stored.size,
            # The whole run over the whole wall clock, which is the figure the
            # service publishes as its measured rate. The interval between two
            # progress lines is the right thing to show while a run is going and
            # the wrong thing to keep: it is one twentieth of a run, and it
            # includes whatever else the machine was doing during it.
            step_ms=elapsed * 1000 / claim.steps if claim.steps else 0.0,
            energy_drift=drift,
        )
        logger.info("finished %s: %d bytes in %.1f s", claim.id, stored.size, elapsed)


async def serve() -> None:
    """Open everything a worker needs and run until it is told to stop."""
    settings = Settings.from_environment()
    async with pool(settings.database_url, maximum=4) as connections:
        storage = Storage(settings)
        await asyncio.to_thread(storage.ensure_bucket)
        worker = Worker(settings, Jobs(connections), storage)

        loop = asyncio.get_running_loop()
        for name in ("SIGINT", "SIGTERM"):
            received = getattr(signal, name, None)
            if received is None:
                continue
            # Not every platform can be told about a signal through the loop.
            # Where it cannot, the default handler still stops the process and
            # the job goes back through the visibility timeout.
            with contextlib.suppress(NotImplementedError):
                loop.add_signal_handler(received, worker.stop)

        await worker.run_forever()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
    use_compatible_event_loop()
    asyncio.run(serve())


if __name__ == "__main__":
    main()
