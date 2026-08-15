"""Telling the client what a run is doing, as it does it.

A run takes minutes, and a progress bar that does not move reads as broken. The
worker writes what it measures into the job's row; the trigger in `schema.sql`
announces the change on a PostgreSQL channel; this listens on that channel once
per API process and hands the change to whichever sockets are watching that job.

One listening connection rather than one per socket. A connection per watcher
would mean a hundred people watching one run holding a hundred PostgreSQL
backends open to be told the same thing, which is a way of making a popular run
the thing that takes the database down.

The payload on the channel is the job's identifier and nothing else, and this
re-reads the row rather than trusting a message. That means a socket always
sends the current state: if three progress reports arrive while one is being
written to a slow connection, the client is sent the latest rather than a queue
of three, which is the right behaviour for a value that supersedes itself.

Polling is the fallback and is not implemented here, because it needs nothing:
`GET /jobs/{id}` answers the same `Job` this pushes. A client that cannot open a
socket asks again, and learns the same things less often.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging

import psycopg

from .contract import Job
from .queue import CHANNEL, Jobs

logger = logging.getLogger(__name__)

#: How long to wait before listening again after the connection dropped.
#:
#: A second. The channel carries progress rather than anything that has to
#: arrive, since every socket sends the whole job and a client that missed a
#: report gets the next one, so this reconnects steadily rather than urgently.
RECONNECT_SECONDS = 1.0


class Progress:
    """The one listener, and the sockets watching through it."""

    def __init__(self, url: str, jobs: Jobs) -> None:
        self._url = url
        self._jobs = jobs
        self._watchers: dict[str, set[asyncio.Queue[Job]]] = {}
        self._task: asyncio.Task[None] | None = None

    async def start(self) -> None:
        self._task = asyncio.create_task(self._listen(), name="progress-listener")

    @property
    def running(self) -> bool:
        """Whether the listener is still there.

        Read by the liveness probe. The listener catches everything it can and
        listens again, so the only way this is false is that it was never
        started or its task ended, and either means progress has stopped
        reaching anybody watching a run.
        """
        return self._task is not None and not self._task.done()

    async def stop(self) -> None:
        if self._task is None:
            return
        self._task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await self._task
        self._task = None

    @contextlib.contextmanager
    def watch(self, identifier: str):
        """A queue of updates for one job, released when the caller is done.

        A context manager because the release matters: a socket that closed
        without unsubscribing would leave a queue nothing reads, and the
        listener would fill it for as long as the job kept changing.
        """
        updates: asyncio.Queue[Job] = asyncio.Queue(maxsize=1)
        self._watchers.setdefault(identifier, set()).add(updates)
        try:
            yield updates
        finally:
            watching = self._watchers.get(identifier)
            if watching is not None:
                watching.discard(updates)
                if not watching:
                    del self._watchers[identifier]

    async def _deliver(self, identifier: str) -> None:
        watching = self._watchers.get(identifier)
        if not watching:
            return

        job = await self._jobs.job(identifier)
        if job is None:
            return

        for updates in list(watching):
            # The queue holds one. A watcher that has not read the last update
            # is behind, and what it wants when it catches up is the current
            # state rather than the one it missed, so the stale entry is thrown
            # away and replaced.
            with contextlib.suppress(asyncio.QueueEmpty):
                updates.get_nowait()
            with contextlib.suppress(asyncio.QueueFull):
                updates.put_nowait(job)

    async def _listen(self) -> None:
        while True:
            try:
                async with await psycopg.AsyncConnection.connect(
                    self._url, autocommit=True
                ) as connection:
                    await connection.execute(f"LISTEN {CHANNEL}")
                    logger.info("listening for job changes")
                    async for notice in connection.notifies():
                        await self._deliver(notice.payload)
            except asyncio.CancelledError:
                raise
            except Exception:
                # Any failure here is the connection, and the answer to all of
                # them is the same: listen again. Logged rather than swallowed,
                # because a service whose progress has silently stopped working
                # looks exactly like a service whose jobs have stopped moving.
                logger.exception("the progress listener stopped; listening again")
                await asyncio.sleep(RECONNECT_SECONDS)
