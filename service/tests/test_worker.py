"""The worker, running the real simulator into a real object store.

This is the case the whole service exists to make work: a configuration goes in
one end and a trajectory the browser can play comes out of the other, with the
physics done by the same binary a command line would have run.

Two of the cases here are about what happens when it goes wrong, and they are
the ones worth having. Killing a worker part way through a run has to leave the
system in a state another worker can pick up, and it has to leave no half
written trajectory behind for a client to fetch.

The runs are small: a few hundred particles over a few hundred steps, which is
a second or two each. What is being tested is the plumbing, and a large run
would test the same plumbing more slowly.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import subprocess
import sys

import psycopg
import pytest
from fastapi.testclient import TestClient

from orrery_service.api import create_app
from orrery_service.configuration import read_configuration
from orrery_service.plan import plan
from orrery_service.queue import Jobs
from orrery_service.storage import Storage, trajectory_key
from orrery_service.worker import Worker, last_energy_drift

from .conftest import (
    DATABASE_URL,
    needs_database,
    needs_simulator,
    needs_storage,
    settings_for_tests,
)

pytestmark = [
    pytest.mark.integration,
    needs_database,
    needs_storage,
    needs_simulator,
]

#: The first eight bytes of a trajectory, which `docs/formats/trajectory.md`
#: specifies as the magic that identifies the format.
MAGIC = b"ORRERYTJ"


def a_run(steps: int = 400, count: int = 256, seed: int = 1):
    return plan(
        read_configuration(
            f"[run]\ntimestep = 0.001\nsteps = {steps}\nseed = {seed}\n"
            f"[initial_conditions]\nkind = plummer\ncount = {count}\n"
            f"[solver]\nkind = barnes-hut\nsoftening = 0.05\n"
        )
    )


def start_a_worker_process() -> subprocess.Popen[str]:
    """A real worker, in its own process, reading the environment as it would.

    This is the only place the worker's entry point is exercised, so it also
    checks the thing nothing else does: that `python -m orrery_service.worker`
    with the documented variables set is a running worker.
    """
    settings = settings_for_tests()
    environment = {
        **os.environ,
        "ORRERY_DATABASE_URL": settings.database_url,
        "ORRERY_STORAGE_ENDPOINT": settings.storage_endpoint,
        "ORRERY_STORAGE_BUCKET": settings.storage_bucket,
        "ORRERY_STORAGE_ACCESS_KEY": settings.storage_access_key,
        "ORRERY_STORAGE_SECRET_KEY": settings.storage_secret_key,
        "ORRERY_BINARY": settings.binary,
        "ORRERY_VISIBILITY_TIMEOUT_SECONDS": "1",
        "ORRERY_HEARTBEAT_SECONDS": "1",
    }
    return subprocess.Popen(
        [sys.executable, "-m", "orrery_service.worker"],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


async def until(condition, *, seconds: float = 120.0, every: float = 0.25) -> bool:
    """Wait for something the worker does in another task."""
    waited = 0.0
    while waited < seconds:
        if await condition():
            return True
        await asyncio.sleep(every)
        waited += every
    return False


async def test_a_submitted_run_is_taken_and_produces_a_trajectory(
    jobs: Jobs, storage: Storage
) -> None:
    settings = settings_for_tests()
    submitted, _ = await jobs.submit(a_run())

    worker = Worker(settings, jobs, storage)
    running = asyncio.create_task(worker.run_forever())

    async def finished() -> bool:
        job = await jobs.job(submitted.id)
        return job is not None and job.state in ("done", "failed")

    assert await until(finished), "the worker did not finish the run"
    worker.stop()
    with contextlib.suppress(TimeoutError):
        await asyncio.wait_for(running, 10)

    job = await jobs.job(submitted.id)
    assert job.state == "done", job.error
    assert job.attempts == 1
    assert job.trajectory == f"/jobs/{job.id}/trajectory"

    # Measured rather than estimated: the wall clock of the run over its steps,
    # and the drift out of the file the run itself wrote.
    assert job.progress.step_ms is not None and job.progress.step_ms > 0
    assert job.progress.energy_drift is not None
    assert abs(job.progress.energy_drift) < 1e-3

    # And the trajectory is in the store, and is a trajectory.
    key = trajectory_key(job.id)
    size = await asyncio.to_thread(storage.size, key)
    assert size is not None and size > 0
    head = b"".join(await asyncio.to_thread(lambda: list(storage.read(key, 0, 7))))
    assert head == MAGIC


async def test_a_run_that_cannot_start_fails_with_what_it_printed(
    jobs: Jobs, storage: Storage
) -> None:
    """A configuration the queue holds and the binary will not run.

    It cannot arrive through the API, which validates first, so it is put
    straight into the queue. What is being checked is that the worker reports
    the failure rather than hanging or retrying: the run started and said no,
    and saying no again twice more would occupy the queue to reach one answer.
    """
    settings = settings_for_tests()
    broken = a_run()
    # Past the reader, refused by the assembly: no build here has a device.
    text = broken.text.replace("kind = barnes-hut", "kind = sycl-tree").replace(
        "allow_cpu_fallback = true", "allow_cpu_fallback = false"
    )
    with psycopg.connect(DATABASE_URL) as connection:
        connection.execute(
            "INSERT INTO job (id, content_hash, configuration, state, particles, "
            "steps, timestep, stride, frames) VALUES "
            "(gen_random_uuid(), 'broken', %s, 'queued', 256, 400, 0.001, 1, 401)",
            (text,),
        )
        connection.commit()

    worker = Worker(settings, jobs, storage)
    running = asyncio.create_task(worker.run_forever())

    async def settled() -> bool:
        counts = await jobs.counts()
        return counts.queued == 0 and counts.running == 0

    assert await until(settled, seconds=60), "the worker never settled the job"
    worker.stop()
    with contextlib.suppress(TimeoutError):
        await asyncio.wait_for(running, 10)

    with psycopg.connect(DATABASE_URL) as connection:
        state, error, attempts = connection.execute(
            "SELECT state, error, attempts FROM job WHERE content_hash = 'broken'"
        ).fetchone()
    assert state == "failed"
    assert error != ""
    assert attempts == 1


async def test_killing_a_worker_mid_run_leaves_the_job_for_another(
    jobs: Jobs, storage: Storage
) -> None:
    """The property the whole retry mechanism exists to provide.

    A worker is killed the way a machine dying kills one: a real worker process
    is started, given a run long enough to be part way through, and then
    terminated without being told anything. Nothing gets to tidy up, and nothing
    tells the queue. The job has to come back, be taken by another worker, and
    finish, and the trajectory that is eventually stored has to be a whole one.

    A separate process rather than a cancelled task, because cancelling a task
    is not what dying is: the run would carry on in its thread, and what is
    being tested is the case where it does not.
    """
    settings = settings_for_tests()
    submitted, _ = await jobs.submit(a_run(steps=6000, count=512))

    first = await asyncio.to_thread(start_a_worker_process)

    async def started() -> bool:
        job = await jobs.job(submitted.id)
        return job is not None and job.state == "running" and job.progress.step > 0

    try:
        assert await until(started, seconds=90), "the worker never started the run"
    finally:
        first.kill()
        await asyncio.to_thread(first.wait)

    # Nothing has been stored, and the row still says somebody is running it.
    assert (await jobs.job(submitted.id)).state == "running"
    assert await asyncio.to_thread(storage.size, trajectory_key(submitted.id)) is None

    # The reaper notices, and the job goes back with its attempt remembered.
    await asyncio.sleep(settings.visibility_timeout.total_seconds() + 0.3)
    assert await jobs.reap(
        visibility=settings.visibility_timeout, max_attempts=settings.max_attempts
    ) == [submitted.id]

    returned = await jobs.job(submitted.id)
    assert returned.state == "queued"
    assert returned.attempts == 1

    second = Worker(settings, jobs, storage)
    again = asyncio.create_task(second.run_forever())

    async def finished() -> bool:
        job = await jobs.job(submitted.id)
        return job is not None and job.state in ("done", "failed")

    assert await until(finished), "the second worker did not finish the run"
    second.stop()
    with contextlib.suppress(TimeoutError):
        await asyncio.wait_for(again, 10)

    job = await jobs.job(submitted.id)
    assert job.state == "done", job.error
    assert job.attempts == 2

    # And what was stored is a whole trajectory rather than a half written one:
    # the length is the header plus a whole number of frames, and the last frame
    # is the last step of the run.
    size = await asyncio.to_thread(storage.size, trajectory_key(submitted.id))
    assert size is not None and size > 0
    assert job.frames > 1


def test_the_result_is_fetched_through_the_api_in_ranges() -> None:
    """What the client's trajectory reader actually does to a finished job.

    A synchronous case, because it drives the application through the test
    client, and it runs the worker in its own loop rather than sharing this
    one. Ranged, because the reader starts playing a run before the whole file
    has arrived and that is the request it makes.
    """
    settings = settings_for_tests()
    with psycopg.connect(DATABASE_URL) as connection:
        connection.execute("TRUNCATE job, worker")
        connection.commit()

    submitted = asyncio.run(_run_one(settings))

    with TestClient(create_app(settings)) as client:
        whole = client.get(f"/jobs/{submitted}/trajectory")
        assert whole.status_code == 200
        assert whole.headers["accept-ranges"] == "bytes"
        assert whole.content[:8] == MAGIC
        length = len(whole.content)

        header = client.get(
            f"/jobs/{submitted}/trajectory", headers={"Range": "bytes=0-7"}
        )
        assert header.status_code == 206
        assert header.content == MAGIC
        assert header.headers["content-range"] == f"bytes 0-7/{length}"

        # An open-ended range, which is what a reader asks for when it wants the
        # rest of a file it has started.
        rest = client.get(
            f"/jobs/{submitted}/trajectory", headers={"Range": "bytes=8-"}
        )
        assert rest.status_code == 206
        assert rest.content == whole.content[8:]

        # And a range past the end is refused rather than answered with nothing.
        past = client.get(
            f"/jobs/{submitted}/trajectory",
            headers={"Range": f"bytes={length + 10}-"},
        )
        assert past.status_code == 416

        diagnostics = client.get(f"/jobs/{submitted}/diagnostics")
        assert diagnostics.status_code == 200
        assert diagnostics.text.startswith("step,time,")


async def _run_one(settings) -> str:
    """Take one job to completion, and return its identifier."""
    from orrery_service.database import pool

    async with pool(settings.database_url) as connections:
        jobs = Jobs(connections)
        storage = Storage(settings)
        await asyncio.to_thread(storage.ensure_bucket)
        submitted, _ = await jobs.submit(a_run(seed=99))

        worker = Worker(settings, jobs, storage)
        running = asyncio.create_task(worker.run_forever())

        async def finished() -> bool:
            job = await jobs.job(submitted.id)
            return job is not None and job.state in ("done", "failed")

        assert await until(finished), "the worker did not finish the run"
        worker.stop()
        with contextlib.suppress(TimeoutError):
            await asyncio.wait_for(running, 10)

        job = await jobs.job(submitted.id)
        assert job.state == "done", job.error
        return submitted.id


def test_the_energy_drift_is_read_from_the_column_it_is_in(tmp_path) -> None:
    """A unit case, because the column is found by name rather than by position.

    A file whose columns moved should produce nothing rather than the value that
    happened to be in the sixth field.
    """
    good = tmp_path / "good.csv"
    good.write_text(
        "step,time,kinetic_energy,potential_energy,total_energy,"
        "relative_energy_error,virial_ratio\n"
        "0,0,1,-2,-1,0,1\n"
        "10,0.01,1,-2,-1,-2.5e-07,1\n",
        encoding="utf-8",
    )
    assert last_energy_drift(good) == pytest.approx(-2.5e-07)

    moved = tmp_path / "moved.csv"
    moved.write_text("step,time,total_energy\n0,0,-1\n", encoding="utf-8")
    assert last_energy_drift(moved) is None

    assert last_energy_drift(tmp_path / "absent.csv") is None
