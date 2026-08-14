"""The job queue, in the database that already holds the jobs.

ADR-0056 records why there is no broker here. The short of it: the queue and the
job metadata are the same rows, `SELECT ... FOR UPDATE SKIP LOCKED` is exactly
the primitive a work queue needs, and a second service to run and reason about
buys nothing at a scale where the whole system is one worker.

What that leaves to get right is the part a broker would have provided, and it
is all in this file:

- **A claim is atomic.** One statement selects the oldest queued row, locks it,
  skips any row another worker has already locked, and moves it to running. Two
  workers claiming at the same moment take two different jobs or one takes
  nothing, and neither can happen half way.
- **A dead worker gives its job back.** The claim is only good while the worker
  keeps saying so. `reap` returns a job whose heartbeat has stopped, which is
  what makes killing a worker mid-run safe rather than a job that is running
  for ever according to a row nobody will change.
- **A job that kills workers is abandoned.** The attempt count is incremented by
  the claim, so a job that has been claimed too many times is not claimed again
  and is failed with a sentence saying why.
- **The same run submitted twice is run once.** The content hash is unique, and
  a submission that collides with one returns the first job.

None of that is visible from outside this module, which hands out the contract's
`Job` and nothing else.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass
from datetime import timedelta
from typing import Any

from psycopg_pool import AsyncConnectionPool

from .contract import Job, Progress
from .plan import Plan

#: The channel the trigger in `schema.sql` announces changes on.
CHANNEL = "orrery_job"

#: Every column a `Job` is built from, plus the queue position.
#:
#: Written once here rather than in each of the four statements that return a
#: job, so that a column added to the row cannot be returned by one and not the
#: others.
_COLUMNS = """
    job.id, job.state, job.created_at, job.attempts, job.particles, job.steps,
    job.timestep, job.stride, job.frames, job.progress_step, job.progress_time,
    job.step_ms, job.energy_drift, job.trajectory_key, job.diagnostics_key,
    job.error,
    CASE WHEN job.state = 'queued' THEN (
        SELECT count(*) FROM job AS ahead
        WHERE ahead.state = 'queued' AND ahead.created_at < job.created_at
    ) END AS position
"""


def _job(row: dict[str, Any]) -> Job:
    """One row, as the contract describes it.

    The two result fields are paths rather than keys. A client is told where to
    fetch a trajectory from this service, and where this service keeps it is
    nothing the client can use or should learn.
    """
    identifier = str(row["id"])
    finished = row["trajectory_key"] is not None
    return Job(
        id=identifier,
        state=row["state"],
        created=row["created_at"].isoformat(),
        position=row["position"],
        attempts=row["attempts"],
        particles=row["particles"],
        steps=row["steps"],
        timestep=row["timestep"],
        stride=row["stride"],
        frames=row["frames"],
        progress=Progress(
            step=row["progress_step"],
            time=row["progress_time"],
            step_ms=row["step_ms"],
            energy_drift=row["energy_drift"],
        ),
        error=row["error"],
        trajectory=f"/jobs/{identifier}/trajectory" if finished else None,
        diagnostics=(
            f"/jobs/{identifier}/diagnostics"
            if row["diagnostics_key"] is not None
            else None
        ),
    )


@dataclass(frozen=True)
class Claim:
    """A job a worker has taken, with what it needs to run it."""

    id: str
    configuration: str
    steps: int
    attempts: int


@dataclass(frozen=True)
class Counts:
    """How much is waiting and how much is being worked on."""

    queued: int
    running: int


class Jobs:
    """Everything that can be done to the queue.

    Takes a pool rather than opening one, because the API and the worker each
    have exactly one and share it with everything else they do.
    """

    def __init__(self, connections: AsyncConnectionPool) -> None:
        self._pool = connections

    async def submit(self, plan: Plan) -> tuple[Job, bool]:
        """Queue `plan`, or return the job that already runs it.

        Returns the job and whether it was already there.

        The insert leaves the decision to the unique constraint rather than
        looking first and inserting after. Two identical submissions arriving
        together would both find nothing and both insert, and one would fail;
        `ON CONFLICT DO NOTHING` makes the second a no-op instead, and whether
        it inserted is what says which of the two happened.

        The row is read back in a second statement rather than in the same one.
        A data-modifying common table expression cannot be read from the table
        it wrote to: the outer query runs against the snapshot taken before the
        statement, so it would see nothing and every submission would look like
        a duplicate. Both statements are in one transaction, and the second is
        after the first, so it sees the insert.
        """
        async with self._pool.connection() as connection:
            inserted = await connection.execute(
                """
                INSERT INTO job (
                    id, content_hash, configuration, state,
                    particles, steps, timestep, stride, frames
                )
                VALUES (%s, %s, %s, 'queued', %s, %s, %s, %s, %s)
                ON CONFLICT (content_hash) DO NOTHING
                RETURNING id
                """,
                (
                    uuid.uuid4(),
                    plan.content_hash,
                    plan.text,
                    plan.particles,
                    plan.steps,
                    plan.configuration.run.timestep,
                    plan.stride,
                    plan.frames,
                ),
            )
            duplicate = await inserted.fetchone() is None

            result = await connection.execute(
                f"SELECT {_COLUMNS} FROM job WHERE content_hash = %s",
                (plan.content_hash,),
            )
            row = await result.fetchone()
            # The row cannot be missing: either it was just inserted or the
            # insert conflicted with it, and nothing deletes jobs. An assertion
            # rather than a fallback, because a fallback here would be code that
            # has never run and cannot be tested.
            assert row is not None, "the job that was just submitted is not there"
            return _job(row), duplicate

    async def claim(self, worker: str, *, max_attempts: int) -> Claim | None:
        """Take the oldest queued job, or return None if there is none to take.

        The whole of the concurrency argument is in this statement. The inner
        select locks one row and skips rows another worker has locked, so two
        workers running this at the same instant cannot come away with the same
        job. `LIMIT 1` is inside the locking select rather than outside it,
        which matters: outside, both workers would lock the same row and one
        would wait for the other rather than skipping to the next.

        The attempt limit appears here as well as in `reap`, which is where a
        job actually gives up. It should never exclude anything: a job reaches
        the limit while it is running, and the reaper fails it in the same
        statement that would have queued it again. The guard is here so that a
        row which somehow got back to queued with its attempts spent cannot be
        picked up for ever by a worker that keeps dying on it.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                WITH taken AS (
                    SELECT id FROM job
                    WHERE state = 'queued' AND attempts < %s
                    ORDER BY created_at
                    FOR UPDATE SKIP LOCKED
                    LIMIT 1
                )
                UPDATE job SET
                    state = 'running',
                    attempts = job.attempts + 1,
                    claimed_at = now(),
                    heartbeat_at = now(),
                    worker = %s
                FROM taken
                WHERE job.id = taken.id
                RETURNING job.id, job.configuration, job.steps, job.attempts
                """,
                (max_attempts, worker),
            )
            row = await result.fetchone()
            if row is None:
                return None
            return Claim(
                id=str(row["id"]),
                configuration=row["configuration"],
                steps=row["steps"],
                attempts=row["attempts"],
            )

    async def beat(
        self,
        identifier: str,
        worker: str,
        *,
        step: int | None = None,
        time: float | None = None,
        step_ms: float | None = None,
        energy_drift: float | None = None,
    ) -> bool:
        """Say the job is still being worked on, and how far it has got.

        Returns whether the beat landed. It does not when the job has been
        reaped and given to somebody else, and a worker that is told so stops:
        continuing would mean two workers producing two trajectories for one
        row, and the second to finish overwriting the first.

        The progress fields are optional and are left alone when absent, so that
        a beat between two of the simulator's progress lines does not reset the
        step count to zero.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                UPDATE job SET
                    heartbeat_at = now(),
                    progress_step = coalesce(%s, progress_step),
                    progress_time = coalesce(%s, progress_time),
                    step_ms = coalesce(%s, step_ms),
                    energy_drift = coalesce(%s, energy_drift)
                WHERE id = %s AND worker = %s AND state = 'running'
                RETURNING id
                """,
                (step, time, step_ms, energy_drift, identifier, worker),
            )
            return await result.fetchone() is not None

    async def finish(
        self,
        identifier: str,
        worker: str,
        *,
        trajectory_key: str,
        diagnostics_key: str,
        trajectory_bytes: int,
        step_ms: float,
        energy_drift: float | None,
    ) -> bool:
        """Record a completed run and where its output went.

        Guarded on the worker still holding the job, for the same reason `beat`
        is: a worker that was reaped part way through must not be able to
        finish a job another worker is now running.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                UPDATE job SET
                    state = 'done',
                    finished_at = now(),
                    heartbeat_at = now(),
                    progress_step = steps,
                    progress_time = steps * timestep,
                    step_ms = %s,
                    energy_drift = coalesce(%s, energy_drift),
                    trajectory_key = %s,
                    diagnostics_key = %s,
                    trajectory_bytes = %s
                WHERE id = %s AND worker = %s AND state = 'running'
                RETURNING id
                """,
                (
                    step_ms,
                    energy_drift,
                    trajectory_key,
                    diagnostics_key,
                    trajectory_bytes,
                    identifier,
                    worker,
                ),
            )
            return await result.fetchone() is not None

    async def fail(self, identifier: str, worker: str, reason: str) -> bool:
        """Record a run that did not finish.

        A failure the worker knows about is final rather than retried. The
        retries this queue does are for workers that disappeared, which is a
        different thing from a run that started and reported an error: the
        second would fail again, and doing it three times would occupy the
        queue for three times as long to reach the same answer.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                UPDATE job SET
                    state = 'failed',
                    finished_at = now(),
                    error = %s
                WHERE id = %s AND worker = %s AND state = 'running'
                RETURNING id
                """,
                (reason, identifier, worker),
            )
            return await result.fetchone() is not None

    async def reap(self, *, visibility: timedelta, max_attempts: int) -> list[str]:
        """Give back the jobs whose workers stopped answering.

        Returns the identifiers of the jobs it moved, which is what makes the
        behaviour observable to a test rather than something inferred from the
        queue getting shorter.

        A job that has been claimed the full number of times is failed rather
        than queued again. The message says what happened, because "the worker
        taking this stopped answering" is a different thing to be told than "the
        run reported an error", and somebody looking at a failed job should not
        have to guess which they have.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                UPDATE job SET
                    state = CASE
                        WHEN attempts >= %(limit)s THEN 'failed' ELSE 'queued'
                    END,
                    worker = '',
                    claimed_at = NULL,
                    heartbeat_at = NULL,
                    finished_at = CASE WHEN attempts >= %(limit)s THEN now() END,
                    error = CASE WHEN attempts >= %(limit)s THEN
                        'the workers taking this run stopped answering '
                        || attempts || ' times, so it was not tried again'
                        ELSE error END
                WHERE state = 'running'
                  AND heartbeat_at < now() - %(visibility)s::interval
                RETURNING id
                """,
                {"limit": max_attempts, "visibility": visibility},
            )
            return [str(row["id"]) async for row in result]

    async def job(self, identifier: str) -> Job | None:
        """One job by its identifier, or None if there is no such job."""
        async with self._pool.connection() as connection:
            try:
                key = uuid.UUID(identifier)
            except ValueError:
                # Not a job that could exist. Answered here rather than left to
                # the database, which would raise on the malformed value and
                # turn a wrong address into an error rather than a not-found.
                return None
            result = await connection.execute(
                f"SELECT {_COLUMNS} FROM job WHERE job.id = %s", (key,)
            )
            row = await result.fetchone()
            return None if row is None else _job(row)

    async def keys(self, identifier: str) -> tuple[str | None, str | None]:
        """Where a finished job's two files live in object storage."""
        async with self._pool.connection() as connection:
            try:
                key = uuid.UUID(identifier)
            except ValueError:
                return None, None
            result = await connection.execute(
                "SELECT trajectory_key, diagnostics_key FROM job WHERE id = %s", (key,)
            )
            row = await result.fetchone()
            if row is None:
                return None, None
            return row["trajectory_key"], row["diagnostics_key"]

    async def counts(self) -> Counts:
        """How many jobs are waiting and how many are being run."""
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                SELECT
                    count(*) FILTER (WHERE state = 'queued') AS queued,
                    count(*) FILTER (WHERE state = 'running') AS running
                FROM job
                """
            )
            row = await result.fetchone()
            assert row is not None
            return Counts(queued=row["queued"], running=row["running"])

    async def reference(self) -> tuple[float, int, int] | None:
        """The median step time this service has achieved, and what it is over.

        Median rather than mean, on the same argument ADR-0019 makes about the
        benchmark protocol: one job that landed on a machine under load should
        not move the figure a client prices with.

        The particle count reported beside it is the median of the counts, so
        the pair is a step time and the size of run it is typical of. That is
        not a measurement of one run and is not offered as one, which is why the
        number of jobs is returned with it.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                SELECT
                    percentile_cont(0.5) WITHIN GROUP (ORDER BY step_ms) AS step_ms,
                    percentile_cont(0.5) WITHIN GROUP (ORDER BY particles) AS particles,
                    count(*) AS jobs
                FROM job
                WHERE state = 'done' AND step_ms IS NOT NULL
                """
            )
            row = await result.fetchone()
            if row is None or row["jobs"] == 0:
                return None
            return row["step_ms"], int(row["particles"]), row["jobs"]

    async def announce(self, worker: str, version: str) -> None:
        """Say that a worker is alive, whether or not it is holding a job."""
        async with self._pool.connection() as connection:
            await connection.execute(
                """
                INSERT INTO worker (name, heartbeat_at, version)
                VALUES (%s, now(), %s)
                ON CONFLICT (name) DO UPDATE
                SET heartbeat_at = now(), version = excluded.version
                """,
                (worker, version),
            )

    async def workers(self, *, visibility: timedelta) -> tuple[int, str]:
        """How many workers have beaten recently, and what they run.

        The version is the one the most recent worker reported, since a
        deployment part way through a rollout has two and the newer one is the
        one worth naming.
        """
        async with self._pool.connection() as connection:
            result = await connection.execute(
                """
                SELECT count(*) AS alive,
                       (SELECT version FROM worker
                        WHERE heartbeat_at > now() - %(visibility)s::interval
                        ORDER BY heartbeat_at DESC LIMIT 1) AS version
                FROM worker
                WHERE heartbeat_at > now() - %(visibility)s::interval
                """,
                {"visibility": visibility},
            )
            row = await result.fetchone()
            assert row is not None
            return row["alive"], row["version"] or ""
