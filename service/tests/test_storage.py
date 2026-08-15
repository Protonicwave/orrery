"""The object store, and the sweep that empties it again.

Against a real S3-compatible store, because what is being asserted is what
boto3 and the store do between them: that deleting something already gone is a
success, that a lifecycle rule is accepted, and that a result removed from the
bucket is a job which says so rather than a link that answers 404.
"""

from __future__ import annotations

from datetime import timedelta
from pathlib import Path

import pytest
from psycopg_pool import AsyncConnectionPool

from orrery_service.api import Service, _sweep_once
from orrery_service.configuration import read_configuration
from orrery_service.limits import RETENTION
from orrery_service.plan import plan
from orrery_service.progress import Progress
from orrery_service.queue import Jobs
from orrery_service.storage import Storage, diagnostics_key, trajectory_key

from .conftest import needs_database, needs_storage, settings_for_tests

pytestmark = [pytest.mark.integration, needs_storage]

CLUSTER = (
    "[run]\ntimestep = 0.001\nsteps = 100\nseed = 3\n"
    "[initial_conditions]\nkind = plummer\ncount = 16\n"
)


def test_deleting_what_is_not_there_is_not_a_failure(
    storage: Storage, tmp_path: Path
) -> None:
    """Which is what makes a sweep safe to run again after it died half way.

    The objects go before the rows are marked, so an interrupted sweep leaves
    rows naming objects that have already gone, and the next pass asks for them
    to be deleted a second time.
    """
    written = tmp_path / "trajectory.otj"
    written.write_bytes(b"not a trajectory, and the store does not mind")
    storage.put_file("jobs/deleted/trajectory.otj", written, "application/octet-stream")

    assert storage.size("jobs/deleted/trajectory.otj") is not None
    storage.delete(["jobs/deleted/trajectory.otj", "jobs/never/existed.otj"])
    assert storage.size("jobs/deleted/trajectory.otj") is None

    # And again, over the same keys, which is the interrupted case exactly.
    storage.delete(["jobs/deleted/trajectory.otj"])


def test_the_bucket_is_asked_to_expire_what_the_sweep_might_miss(
    storage: Storage,
) -> None:
    assert storage.ensure_lifecycle(RETENTION) is True
    # Twice, because it is applied on every start-up.
    assert storage.ensure_lifecycle(RETENTION) is True


@needs_database
async def test_a_swept_run_keeps_its_row_and_loses_its_result(
    connections: AsyncConnectionPool, storage: Storage, tmp_path: Path
) -> None:
    """The whole of the expiry, from a finished job to one that says so."""
    jobs = Jobs(connections)
    settings = settings_for_tests()
    job, _ = await jobs.submit(plan(read_configuration(CLUSTER)))
    claim = await jobs.claim("worker-1", max_attempts=3, max_running=2)
    assert claim is not None

    trajectory = tmp_path / "trajectory.otj"
    diagnostics = tmp_path / "diagnostics.csv"
    trajectory.write_bytes(b"0" * 1024)
    diagnostics.write_text("step,relative_energy_error\n0,0\n", encoding="utf-8")
    storage.put_file(trajectory_key(job.id), trajectory, "application/octet-stream")
    storage.put_file(diagnostics_key(job.id), diagnostics, "text/csv")

    await jobs.finish(
        job.id,
        "worker-1",
        trajectory_key=trajectory_key(job.id),
        diagnostics_key=diagnostics_key(job.id),
        trajectory_bytes=1024,
        step_ms=1.0,
        energy_drift=0.0,
    )

    service = Service(
        settings=settings,
        jobs=jobs,
        storage=storage,
        progress=Progress(settings.database_url, jobs),
    )

    # Nothing is old enough at the deployment's retention, and everything is at
    # a retention of nothing.
    assert await _sweep_once(service) == 0
    assert storage.size(trajectory_key(job.id)) is not None

    assert await _sweep_once(service, after=timedelta(seconds=0)) == 1
    assert storage.size(trajectory_key(job.id)) is None
    assert storage.size(diagnostics_key(job.id)) is None

    read = await jobs.job(job.id)
    assert read is not None
    assert read.state == "done"
    assert read.trajectory is None

    # And the sweep does not offer it again, whatever it is asked for.
    assert await _sweep_once(service, after=timedelta(seconds=0)) == 0
