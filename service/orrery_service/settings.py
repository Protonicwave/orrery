"""Where the service finds the things it does not contain.

The database, the object store and the simulator are all outside this process,
and every one of them is named by an environment variable with no default that
would work by accident. A service that fell back to a local PostgreSQL when the
variable was missing would come up, appear healthy, and write jobs somewhere
nobody was reading them.

The one exception is the read-only settings that decide behaviour rather than
location: the visibility timeout, the retry limit and the heartbeat interval.
Those have defaults because there is a right answer for them and it does not
depend on where the service is deployed.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from datetime import timedelta


class SettingsError(RuntimeError):
    """A variable the service cannot start without is missing or unreadable."""


def _required(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise SettingsError(
            f"{name} is not set. The service has no default for it, because a "
            f"default would be a working process pointed at the wrong place."
        )
    return value


def _integer(name: str, fallback: int) -> int:
    text = os.environ.get(name, "").strip()
    if not text:
        return fallback
    try:
        return int(text)
    except ValueError as error:
        raise SettingsError(f"{name} is not a whole number: {text!r}") from error


@dataclass(frozen=True)
class Settings:
    """Everything the service reads out of its environment."""

    #: A libpq connection string for the metadata database and the queue.
    database_url: str

    #: The S3-compatible endpoint trajectories are written to, its bucket and
    #: its credentials. Region is required by the signing algorithm rather than
    #: by any particular store, so it has a default that means nothing.
    storage_endpoint: str
    storage_bucket: str
    storage_access_key: str
    storage_secret_key: str
    storage_region: str

    #: The native simulator. A path rather than a name on PATH, so that a worker
    #: cannot pick up a different build of it than the one its image carries.
    binary: str

    #: How long a claimed job may go without a heartbeat before another worker
    #: may take it.
    #:
    #: Ninety seconds, which is several times the interval the worker beats at
    #: and long enough to survive a garbage collection or a slow upload. Shorter
    #: and a healthy worker loses its own job; much longer and a machine that
    #: has died holds a job past the point anybody is still waiting.
    visibility_timeout: timedelta

    #: How often a worker says it is still there.
    heartbeat_interval: timedelta

    #: How many times a job may be claimed before it is abandoned.
    #:
    #: Three. A job that has killed two workers is more likely to be a job that
    #: kills workers than a job that met two unlucky machines, and retrying it
    #: for ever would be a queue that never drains.
    max_attempts: int

    @staticmethod
    def from_environment() -> Settings:
        return Settings(
            database_url=_required("ORRERY_DATABASE_URL"),
            storage_endpoint=_required("ORRERY_STORAGE_ENDPOINT"),
            storage_bucket=_required("ORRERY_STORAGE_BUCKET"),
            storage_access_key=_required("ORRERY_STORAGE_ACCESS_KEY"),
            storage_secret_key=_required("ORRERY_STORAGE_SECRET_KEY"),
            storage_region=os.environ.get("ORRERY_STORAGE_REGION", "us-east-1"),
            binary=os.environ.get("ORRERY_BINARY", "orrery"),
            visibility_timeout=timedelta(
                seconds=_integer("ORRERY_VISIBILITY_TIMEOUT_SECONDS", 90)
            ),
            heartbeat_interval=timedelta(
                seconds=_integer("ORRERY_HEARTBEAT_SECONDS", 15)
            ),
            max_attempts=_integer("ORRERY_MAX_ATTEMPTS", 3),
        )
