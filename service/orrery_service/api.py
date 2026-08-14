"""The HTTP and WebSocket surface.

Seven routes, and every one of them is thin. Submitting is validate, plan,
insert; asking is one query; watching is a queue of the same answer; fetching a
result is a range of an object copied out of the store. Nothing here decides
anything: what a submission may ask for is `limits`, whether it is a run is
`validation`, what it becomes is `plan`, and what happens to it is `queue`.

    POST   /jobs                    submit a run
    GET    /jobs/{id}               how it is going
    GET    /jobs/{id}/trajectory    the result, ranged
    GET    /jobs/{id}/diagnostics   the conserved quantities it wrote
    WS     /jobs/{id}/progress      the same job, pushed as it changes
    GET    /capabilities            what the service will take, right now
    GET    /health                  alive, and ready

The reaper runs in this process rather than in the worker. A worker that has
died cannot return its own job, so the thing that notices has to be something
else, and the API is the process that is always up.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import re
from collections.abc import AsyncIterator, Iterator
from dataclasses import dataclass

from fastapi import FastAPI, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse

from . import __version__, limits
from .contract import (
    Accepted,
    Capabilities,
    Health,
    Job,
    Limits,
    Problem,
    Reference,
    Rejection,
    Submission,
)
from .database import pool, use_compatible_event_loop
from .plan import plan
from .progress import Progress
from .queue import Jobs
from .settings import Settings
from .storage import Storage, StorageError
from .validation import check_submission

logger = logging.getLogger(__name__)

#: A byte range as a client asks for it. Only the single-range forms, which is
#: all the trajectory reader in `web/src/trajectory/source.ts` sends and all
#: this needs to answer: multipart ranges would be a second response format to
#: get right for a case nothing produces.
_RANGE = re.compile(r"^bytes=(\d*)-(\d*)$")


@dataclass
class Service:
    """What the routes work through, assembled once when the process starts."""

    settings: Settings
    jobs: Jobs
    storage: Storage
    progress: Progress


def _limits() -> Limits:
    return Limits(
        max_particles=limits.MAX_PARTICLES,
        max_steps=limits.MAX_STEPS,
        max_work=limits.MAX_WORK,
        max_queue=limits.MAX_QUEUE,
        solvers=list(limits.ALLOWED_SOLVERS),
    )


async def _capabilities(service: Service) -> Capabilities:
    counts = await service.jobs.counts()
    alive, version = await service.jobs.workers(
        visibility=service.settings.visibility_timeout
    )
    measured = await service.jobs.reference()

    return Capabilities(
        # What a worker reported, where one has. This package's own version is
        # the fallback and is the less useful answer: what matters to somebody
        # reading a trajectory is the simulator that produced it.
        version=version or __version__,
        limits=_limits(),
        # Both conditions, because they are different failures with the same
        # answer for the client: there is nothing to take a run. ADR-0054.
        accepting=alive > 0 and counts.queued < limits.MAX_QUEUE,
        queued=counts.queued,
        workers=alive,
        reference=(
            None
            if measured is None
            else Reference(step_ms=measured[0], particles=measured[1], jobs=measured[2])
        ),
    )


async def _reap(service: Service) -> None:
    """Return the jobs of workers that have stopped answering, for ever.

    Every third of the visibility timeout, so that a job is back in the queue
    within about a third of it after the worker holding it died rather than
    whenever somebody next submitted something.
    """
    interval = service.settings.visibility_timeout.total_seconds() / 3
    while True:
        try:
            await asyncio.sleep(interval)
            returned = await service.jobs.reap(
                visibility=service.settings.visibility_timeout,
                max_attempts=service.settings.max_attempts,
            )
            if returned:
                logger.info("returned %d job(s) to the queue", len(returned))
        except asyncio.CancelledError:
            raise
        except Exception:
            # The database being briefly unreachable is not a reason to stop
            # reaping for the life of the process.
            logger.exception("the reaper failed; trying again")


def create_app(settings: Settings | None = None) -> FastAPI:
    """The application, with everything it needs opened around its lifetime.

    Takes its settings rather than reading the environment, so that a test can
    point one at a scratch database without setting variables in the process it
    is running in.
    """
    configured = settings or Settings.from_environment()

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI) -> AsyncIterator[None]:
        async with pool(configured.database_url) as connections:
            jobs = Jobs(connections)
            storage = Storage(configured)
            # Blocking, and deliberately so: a service that came up without
            # somewhere to put results would accept runs it could not finish.
            await asyncio.to_thread(storage.ensure_bucket)

            service = Service(
                settings=configured,
                jobs=jobs,
                storage=storage,
                progress=Progress(configured.database_url, jobs),
            )
            await service.progress.start()
            reaper = asyncio.create_task(_reap(service), name="reaper")

            app.state.service = service
            try:
                yield
            finally:
                reaper.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await reaper
                await service.progress.stop()

    app = FastAPI(
        title="Orrery compute service",
        version=__version__,
        summary="Submit an N-body run, watch it, and play the result.",
        lifespan=lifespan,
    )

    if configured.allowed_origins:
        app.add_middleware(
            CORSMiddleware,
            allow_origins=list(configured.allowed_origins),
            allow_methods=["GET", "POST"],
            allow_headers=["Content-Type", "Range"],
            # The client reads Content-Range to learn how long a trajectory is
            # before it has fetched it, and a cross-origin response hides every
            # header that is not named here.
            expose_headers=["Content-Range", "Content-Length", "Accept-Ranges"],
        )

    def service_of(request: Request) -> Service:
        return request.app.state.service

    @app.get("/capabilities", response_model=Capabilities)
    async def capabilities(request: Request) -> Capabilities:
        return await _capabilities(service_of(request))

    @app.get("/health", response_model=Health)
    async def health(request: Request) -> Response:
        """Alive is this process answering. Ready is it being able to work.

        Kept apart so that an orchestrator can restart a process that is up and
        cannot reach its database, rather than sending it traffic.
        """
        service = service_of(request)
        try:
            await service.jobs.counts()
        except Exception as error:
            return JSONResponse(
                status_code=503,
                content=Health(
                    alive=True, ready=False, detail=f"the database: {error}"
                ).model_dump(),
            )
        return JSONResponse(
            content=Health(alive=True, ready=True, detail="").model_dump()
        )

    @app.post("/jobs", response_model=Accepted)
    async def submit(request: Request, submission: Submission) -> Response:
        service = service_of(request)

        configuration, problems = check_submission(submission.configuration)
        if configuration is None:
            # 422 rather than 400: the request is well formed and its content is
            # a document this service will not run, which is what that code is
            # for. The body is every objection at once.
            return JSONResponse(
                status_code=422, content=Rejection(problems=problems).model_dump()
            )

        capability = await _capabilities(service)
        if not capability.accepting:
            reason = (
                f"the queue is full at {capability.queued} runs"
                if capability.workers > 0
                else "no worker is available to take a run"
            )
            # Refused rather than queued for later. A job accepted into a queue
            # nothing is draining is worse than a refusal, because the person
            # who submitted it waits instead of being told to look at the
            # gallery. ADR-0054.
            return JSONResponse(
                status_code=503,
                content=Rejection(
                    problems=[
                        Problem(
                            setting="service",
                            complaint=(
                                f"is not taking runs at the moment: {reason}. The "
                                f"published runs in the gallery are unaffected"
                            ),
                        )
                    ]
                ).model_dump(),
            )

        job, duplicate = await service.jobs.submit(plan(configuration))
        return JSONResponse(
            status_code=200 if duplicate else 202,
            content=Accepted(job=job, duplicate=duplicate).model_dump(),
        )

    @app.get("/jobs/{identifier}", response_model=Job)
    async def read(request: Request, identifier: str) -> Response:
        job = await service_of(request).jobs.job(identifier)
        if job is None:
            return JSONResponse(status_code=404, content={"detail": "no such job"})
        return JSONResponse(content=job.model_dump())

    def _serve(
        service: Service, key: str, request: Request, media_type: str, name: str
    ) -> Response:
        """A stored object, answering a range request over it.

        Ranged because that is how the trajectory reader fetches: it starts
        playing a run before the whole file has arrived, and it does that by
        asking for the header and then for frames. A server that ignored the
        range would be correct and would cost the reader the download it exists
        to avoid.

        Synchronous, and handed to Starlette as a synchronous iterator, which
        runs it in a thread. boto3 has no async client and does not need one
        here: this is one download at a time on a service sized for one worker.
        """
        length = service.storage.size(key)
        if length is None:
            return JSONResponse(
                status_code=404, content={"detail": f"the {name} is not in storage"}
            )

        header = request.headers.get("range", "")
        start, end = 0, length - 1
        partial = False

        match = _RANGE.match(header.strip()) if header else None
        if match is not None:
            first, last = match.group(1), match.group(2)
            if first == "" and last == "":
                match = None
            elif first == "":
                # A suffix range: the last N bytes.
                start = max(0, length - int(last))
            else:
                start = int(first)
                end = length - 1 if last == "" else min(int(last), length - 1)
            if match is not None:
                partial = True

        if partial and (start >= length or start > end):
            return Response(
                status_code=416, headers={"Content-Range": f"bytes */{length}"}
            )

        def body() -> Iterator[bytes]:
            try:
                yield from service.storage.read(key, start, end)
            except StorageError:
                # The connection is already open and the status already sent, so
                # there is nowhere to report this to the client but the log. The
                # short body is what it sees, and the reader treats a file that
                # ends early as a run it can play up to where it stopped.
                logger.exception("reading %s stopped part way", key)

        headers = {
            "Accept-Ranges": "bytes",
            "Content-Length": str(end - start + 1),
        }
        if partial:
            headers["Content-Range"] = f"bytes {start}-{end}/{length}"

        return StreamingResponse(
            body(),
            status_code=206 if partial else 200,
            media_type=media_type,
            headers=headers,
        )

    @app.get("/jobs/{identifier}/trajectory")
    async def trajectory(request: Request, identifier: str) -> Response:
        service = service_of(request)
        key, _ = await service.jobs.keys(identifier)
        if key is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "this job has not produced a trajectory"},
            )
        return _serve(service, key, request, "application/octet-stream", "trajectory")

    @app.get("/jobs/{identifier}/diagnostics")
    async def diagnostics(request: Request, identifier: str) -> Response:
        service = service_of(request)
        _, key = await service.jobs.keys(identifier)
        if key is None:
            return JSONResponse(
                status_code=404,
                content={"detail": "this job has not produced any diagnostics"},
            )
        return _serve(service, key, request, "text/csv", "diagnostics")

    @app.websocket("/jobs/{identifier}/progress")
    async def watch(socket: WebSocket, identifier: str) -> None:
        """The job, pushed whenever it changes, until it is finished.

        The current state is sent as soon as the socket opens rather than only
        on the next change, because a job that is queued behind five others will
        not change for several minutes and a client that saw nothing in that
        time could not tell a working socket from a broken one.

        The socket closes itself when the job reaches a state it will not leave.
        A client that wants the result then fetches it, and one that has gone
        away has already been noticed by the send failing.
        """
        service = socket.app.state.service
        await socket.accept()

        job = await service.jobs.job(identifier)
        if job is None:
            await socket.close(code=4404, reason="no such job")
            return

        with service.progress.watch(identifier) as updates:
            try:
                await socket.send_json(job.model_dump())
                while job.state in ("queued", "running"):
                    job = await updates.get()
                    await socket.send_json(job.model_dump())
            except WebSocketDisconnect:
                return
            finally:
                with contextlib.suppress(RuntimeError):
                    await socket.close()

    return app


def main() -> None:
    """Run the API under uvicorn, for a container's entry point."""
    import uvicorn

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
    use_compatible_event_loop()
    uvicorn.run(create_app(), host="0.0.0.0", port=8000)


if __name__ == "__main__":
    main()
