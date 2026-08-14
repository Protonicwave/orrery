"""What the integration tests need, and how they skip when it is not there.

Two environments, both named rather than discovered:

- `ORRERY_TEST_DATABASE_URL` is a PostgreSQL the tests may create tables in and
  empty between cases. Nothing here is safe to point at a database anybody
  cares about, and the variable is separate from the service's own
  `ORRERY_DATABASE_URL` so that a shell configured to run the service cannot
  have its database emptied by running the suite in it.
- `ORRERY_TEST_STORAGE_ENDPOINT`, with its bucket and credentials, is an
  S3-compatible store the tests may write to.

`deploy/compose.yaml` brings both up, and `service/README.md` gives the
commands. Without them the integration cases skip with the reason, and the unit
suite, which is most of it, runs on any machine.
"""

from __future__ import annotations

import os
import shutil
from collections.abc import AsyncIterator
from datetime import timedelta
from pathlib import Path

import pytest
from psycopg_pool import AsyncConnectionPool

from orrery_service.database import pool, use_compatible_event_loop
from orrery_service.queue import Jobs
from orrery_service.settings import Settings
from orrery_service.storage import Storage

DATABASE_URL = os.environ.get("ORRERY_TEST_DATABASE_URL", "").strip()
STORAGE_ENDPOINT = os.environ.get("ORRERY_TEST_STORAGE_ENDPOINT", "").strip()

REPOSITORY = Path(__file__).resolve().parents[2]


def simulator() -> str | None:
    """The native binary to run, or None if this machine has not built one.

    ORRERY_BINARY first, so that continuous integration and a container can say
    exactly which build is being used. Otherwise the build trees the presets
    write to, newest first, because somebody working on this locally has usually
    just built one.
    """
    named = os.environ.get("ORRERY_BINARY", "").strip()
    if named:
        return named if Path(named).exists() or shutil.which(named) else None

    found = [
        path
        for pattern in ("build/*/apps/orrery", "build/*/apps/orrery.exe")
        for path in REPOSITORY.glob(pattern)
    ]
    if not found:
        return None
    return str(max(found, key=lambda path: path.stat().st_mtime))


BINARY = simulator()

needs_simulator = pytest.mark.skipif(
    BINARY is None,
    reason=(
        "no orrery binary was found. Build one with "
        "`cmake --build --preset release`, or name it in ORRERY_BINARY."
    ),
)

needs_database = pytest.mark.skipif(
    not DATABASE_URL,
    reason=(
        "ORRERY_TEST_DATABASE_URL is not set. `docker compose -f "
        "deploy/compose.yaml up -d postgres` brings one up."
    ),
)

needs_storage = pytest.mark.skipif(
    not STORAGE_ENDPOINT,
    reason=(
        "ORRERY_TEST_STORAGE_ENDPOINT is not set. `docker compose -f "
        "deploy/compose.yaml up -d storage` brings one up."
    ),
)


# The suite is run on whatever machine somebody is working on, and psycopg's
# async connections do not work on every platform's default event loop. The
# service's two entry points call this for the same reason.
use_compatible_event_loop()


def settings_for_tests() -> Settings:
    """The service's settings, pointed at whatever the environment names.

    A plain function rather than a fixture, because the worker builds one of
    these for itself and threading a fixture into it would be indirection for
    one caller.

    The two intervals are short. A visibility timeout of ninety seconds is right
    for a deployment and would make the reaping tests take ninety seconds each,
    so the tests choose their own and the service's default is left where it
    belongs, in `Settings.from_environment`.
    """
    return Settings(
        database_url=DATABASE_URL,
        storage_endpoint=STORAGE_ENDPOINT,
        storage_bucket=os.environ.get("ORRERY_TEST_STORAGE_BUCKET", "orrery-test"),
        storage_access_key=os.environ.get("ORRERY_TEST_STORAGE_ACCESS_KEY", "orrery"),
        storage_secret_key=os.environ.get(
            "ORRERY_TEST_STORAGE_SECRET_KEY", "orrery123"
        ),
        storage_region="us-east-1",
        binary=BINARY or "orrery",
        visibility_timeout=timedelta(seconds=1),
        heartbeat_interval=timedelta(seconds=0.2),
        max_attempts=3,
        allowed_origins=("http://localhost:5173",),
    )


@pytest.fixture
async def connections() -> AsyncIterator[AsyncConnectionPool]:
    """A pool over the test database, emptied before the case runs.

    Before rather than after, so that a failing case leaves its rows behind to
    be looked at.
    """
    async with pool(DATABASE_URL) as connections:
        async with connections.connection() as connection:
            await connection.execute("TRUNCATE job, worker")
            await connection.commit()
        yield connections


@pytest.fixture
def jobs(connections: AsyncConnectionPool) -> Jobs:
    return Jobs(connections)


@pytest.fixture
def storage() -> Storage:
    store = Storage(settings_for_tests())
    store.ensure_bucket()
    return store
