"""The queue, against a real PostgreSQL.

The cases that matter are the concurrent ones, and they are the reason this
suite refuses to run against anything but a live database: `FOR UPDATE SKIP
LOCKED` is a statement about what two transactions do to each other, and nothing
that stands in for a database reproduces it.

Four properties are asserted here, and they are the four `queue.py` exists to
provide: a job is claimed once, a dead worker gives its job back, a job that
kills workers is abandoned, and the same run submitted twice is run once.
"""

from __future__ import annotations

import asyncio
from datetime import timedelta

import pytest

from orrery_service.configuration import read_configuration
from orrery_service.plan import plan
from orrery_service.queue import Jobs

from .conftest import needs_database

pytestmark = [pytest.mark.integration, needs_database]

VISIBILITY = timedelta(seconds=1)


def a_run(seed: int = 1, steps: int = 1000) -> object:
    return plan(
        read_configuration(
            f"[run]\ntimestep = 0.001\nsteps = {steps}\nseed = {seed}\n"
            f"[initial_conditions]\nkind = plummer\ncount = 64\n"
        )
    )


async def test_a_submitted_job_is_queued_and_says_where_it_is(jobs: Jobs) -> None:
    job, duplicate = await jobs.submit(a_run())
    assert not duplicate
    assert job.state == "queued"
    assert job.position == 0
    assert job.particles == 64
    assert job.steps == 1000
    assert job.trajectory is None


async def test_the_queue_position_counts_what_is_ahead(jobs: Jobs) -> None:
    first, _ = await jobs.submit(a_run(seed=1))
    second, _ = await jobs.submit(a_run(seed=2))
    third, _ = await jobs.submit(a_run(seed=3))
    assert [first.position, second.position, third.position] == [0, 1, 2]

    # Taking the first moves everything behind it up, and a running job has no
    # position because there is nothing meaningful to say.
    await jobs.claim("worker-1", max_attempts=3)
    assert (await jobs.job(first.id)).position is None
    assert (await jobs.job(second.id)).position == 0
    assert (await jobs.job(third.id)).position == 1


async def test_the_same_run_submitted_twice_is_one_job(jobs: Jobs) -> None:
    first, duplicate = await jobs.submit(a_run())
    assert not duplicate
    second, duplicate = await jobs.submit(a_run())
    assert duplicate
    assert second.id == first.id

    counts = await jobs.counts()
    assert counts.queued == 1


async def test_the_same_run_spelled_differently_is_still_one_job(jobs: Jobs) -> None:
    first, _ = await jobs.submit(a_run())
    spelled = plan(
        read_configuration(
            "# A comment the first one did not have.\n"
            "[initial_conditions]\n"
            "count    = 64\n"
            "kind     = plummer\n"
            "[run]\n"
            "seed     = 1\n"
            "steps    = 1000\n"
            "timestep = 0.001\n"
        )
    )
    second, duplicate = await jobs.submit(spelled)
    assert duplicate
    assert second.id == first.id


async def test_a_job_is_claimed_in_order(jobs: Jobs) -> None:
    first, _ = await jobs.submit(a_run(seed=1))
    await jobs.submit(a_run(seed=2))
    claim = await jobs.claim("worker-1", max_attempts=3)
    assert claim is not None
    assert claim.id == first.id
    assert claim.attempts == 1
    # The configuration it hands back is the settled one, output included, so
    # the worker has nothing left to decide.
    assert "trajectory_path = trajectory.otj" in claim.configuration


async def test_an_empty_queue_claims_nothing(jobs: Jobs) -> None:
    assert await jobs.claim("worker-1", max_attempts=3) is None


async def test_no_job_is_claimed_twice(jobs: Jobs) -> None:
    """Twenty workers, ten jobs, all claiming at once.

    This is the case the whole design of the claim is for. Without SKIP LOCKED
    the twenty transactions would queue behind each other on the same row and
    the losers would take the job the winner had just taken; without the LIMIT
    being inside the locking select they would block rather than skip.

    Asserting that the claims are distinct is the point, and asserting that
    there are exactly ten of them is what says the ten that failed failed by
    finding nothing rather than by erroring.
    """
    for seed in range(10):
        await jobs.submit(a_run(seed=seed))

    claims = await asyncio.gather(
        *(jobs.claim(f"worker-{index}", max_attempts=3) for index in range(20))
    )

    taken = [claim for claim in claims if claim is not None]
    assert len(taken) == 10
    assert len({claim.id for claim in taken}) == 10

    counts = await jobs.counts()
    assert counts.queued == 0
    assert counts.running == 10


async def test_progress_is_recorded_and_read_back(jobs: Jobs) -> None:
    job, _ = await jobs.submit(a_run())
    await jobs.claim("worker-1", max_attempts=3)

    assert await jobs.beat(job.id, "worker-1", step=100, time=0.1)
    read = await jobs.job(job.id)
    assert read.state == "running"
    assert read.progress.step == 100
    assert read.progress.time == pytest.approx(0.1)
    assert read.progress.step_ms is None

    # A beat that carries no progress leaves what was there rather than
    # resetting it, so a heartbeat between two of the run's progress lines does
    # not take the step count back to zero.
    assert await jobs.beat(job.id, "worker-1")
    read = await jobs.job(job.id)
    assert read.progress.step == 100


async def test_a_worker_that_does_not_hold_a_job_cannot_change_it(jobs: Jobs) -> None:
    job, _ = await jobs.submit(a_run())
    await jobs.claim("worker-1", max_attempts=3)

    assert not await jobs.beat(job.id, "worker-2", step=50)
    assert not await jobs.fail(job.id, "worker-2", "not mine to fail")
    assert (await jobs.job(job.id)).state == "running"


async def test_a_finished_job_says_where_its_result_is(jobs: Jobs) -> None:
    job, _ = await jobs.submit(a_run())
    await jobs.claim("worker-1", max_attempts=3)

    assert await jobs.finish(
        job.id,
        "worker-1",
        trajectory_key="jobs/x/trajectory.otj",
        diagnostics_key="jobs/x/diagnostics.csv",
        trajectory_bytes=1024,
        step_ms=1.5,
        energy_drift=-1e-9,
    )
    read = await jobs.job(job.id)
    assert read.state == "done"
    assert read.trajectory == f"/jobs/{job.id}/trajectory"
    assert read.diagnostics == f"/jobs/{job.id}/diagnostics"
    # A finished run is at its own end, whatever the last progress line said.
    assert read.progress.step == read.steps
    assert read.progress.step_ms == pytest.approx(1.5)


async def test_a_failure_the_worker_reports_is_final(jobs: Jobs) -> None:
    job, _ = await jobs.submit(a_run())
    await jobs.claim("worker-1", max_attempts=3)
    assert await jobs.fail(job.id, "worker-1", "the run stopped: out of memory")

    read = await jobs.job(job.id)
    assert read.state == "failed"
    assert "out of memory" in read.error
    # Not retried. A run that started and reported an error would report it
    # again, and doing that three times occupies the queue to reach one answer.
    assert await jobs.claim("worker-2", max_attempts=3) is None


async def test_a_job_whose_worker_stopped_answering_goes_back(jobs: Jobs) -> None:
    job, _ = await jobs.submit(a_run())
    claim = await jobs.claim("worker-1", max_attempts=3)
    assert claim is not None

    # Nothing has expired yet, so nothing is reaped.
    assert await jobs.reap(visibility=VISIBILITY, max_attempts=3) == []

    await asyncio.sleep(VISIBILITY.total_seconds() + 0.2)
    assert await jobs.reap(visibility=VISIBILITY, max_attempts=3) == [job.id]

    read = await jobs.job(job.id)
    assert read.state == "queued"
    assert read.position == 0
    # The attempt is remembered, which is what eventually stops it.
    assert read.attempts == 1

    # And another worker can now take it.
    again = await jobs.claim("worker-2", max_attempts=3)
    assert again is not None
    assert again.id == job.id
    assert again.attempts == 2


async def test_a_beat_from_a_reaped_worker_is_refused(jobs: Jobs) -> None:
    """The property that stops two trajectories being written for one job.

    A worker that was reaped and did not notice would otherwise carry on
    running, finish, and overwrite the result of the worker that took over.
    """
    job, _ = await jobs.submit(a_run())
    await jobs.claim("worker-1", max_attempts=3)
    await asyncio.sleep(VISIBILITY.total_seconds() + 0.2)
    await jobs.reap(visibility=VISIBILITY, max_attempts=3)
    await jobs.claim("worker-2", max_attempts=3)

    assert not await jobs.beat(job.id, "worker-1", step=500)
    assert not await jobs.finish(
        job.id,
        "worker-1",
        trajectory_key="k",
        diagnostics_key="d",
        trajectory_bytes=1,
        step_ms=1.0,
        energy_drift=None,
    )
    assert (await jobs.job(job.id)).trajectory is None


async def test_a_job_that_kills_workers_is_given_up_on(jobs: Jobs) -> None:
    job, _ = await jobs.submit(a_run())

    for attempt in range(1, 4):
        claim = await jobs.claim(f"worker-{attempt}", max_attempts=3)
        assert claim is not None, f"attempt {attempt} could not be claimed"
        assert claim.attempts == attempt
        await asyncio.sleep(VISIBILITY.total_seconds() + 0.2)
        assert await jobs.reap(visibility=VISIBILITY, max_attempts=3) == [job.id]

    read = await jobs.job(job.id)
    assert read.state == "failed"
    assert "stopped answering" in read.error
    assert await jobs.claim("worker-4", max_attempts=3) is None


async def test_an_unknown_job_is_not_an_error(jobs: Jobs) -> None:
    assert await jobs.job("d3adbeef-0000-0000-0000-000000000000") is None
    # A wrong address rather than a job that has gone: answered here rather than
    # raising out of the database on a value that is not a UUID at all.
    assert await jobs.job("not-a-uuid") is None


async def test_the_reference_is_absent_until_something_has_run(jobs: Jobs) -> None:
    assert await jobs.reference() is None

    for seed, step_ms in ((1, 2.0), (2, 4.0), (3, 6.0)):
        job, _ = await jobs.submit(a_run(seed=seed))
        await jobs.claim("worker-1", max_attempts=3)
        await jobs.finish(
            job.id,
            "worker-1",
            trajectory_key="k",
            diagnostics_key="d",
            trajectory_bytes=1,
            step_ms=step_ms,
            energy_drift=None,
        )

    reference = await jobs.reference()
    assert reference is not None
    step_ms, particles, count = reference
    # The median rather than the mean, so one slow run does not move it.
    assert step_ms == pytest.approx(4.0)
    assert particles == 64
    assert count == 3


async def test_a_worker_says_it_is_alive_whether_or_not_it_holds_a_job(
    jobs: Jobs,
) -> None:
    assert await jobs.workers(visibility=VISIBILITY) == (0, "")

    await jobs.announce("worker-1", "1.0.0")
    assert await jobs.workers(visibility=VISIBILITY) == (1, "1.0.0")

    # Announcing again is the same worker, not a second one.
    await jobs.announce("worker-1", "1.0.0")
    await jobs.announce("worker-2", "1.0.0")
    alive, _ = await jobs.workers(visibility=VISIBILITY)
    assert alive == 2

    # And one that stops beating stops counting, which is what lets the client
    # be told there is nothing to take a run.
    await asyncio.sleep(VISIBILITY.total_seconds() + 0.2)
    assert await jobs.workers(visibility=VISIBILITY) == (0, "")
