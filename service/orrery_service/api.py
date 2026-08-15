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
    GET    /health                  is this process working
    GET    /ready                   can it reach what it needs
    GET    /metrics                 what it has done, and what is queued

The reaper runs in this process rather than in the worker. A worker that has
died cannot return its own job, so the thing that notices has to be something
else, and the API is the process that is always up. The sweep that expires
stored results runs here for the same reason.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import re
import time
import uuid
from collections.abc import AsyncIterator, Iterator
from dataclasses import dataclass, field

from fastapi import FastAPI, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse

from . import __version__, limits
from .access import address_of, submitter_of
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
from .observability import (
    REQUEST_ID,
    clean_request_id,
    configure_logging,
    context,
    count,
    render,
)
from .plan import plan
from .progress import Progress
from .queue import Jobs
from .settings import Settings
from .storage import Storage, StorageError, diagnostics_key, trajectory_key
from .validation import check_submission

logger = logging.getLogger(__name__)

#: Both objects a finished run leaves behind, as functions of its identifier.
#:
#: Named here so the sweep removes everything a job wrote rather than the file
#: whoever wrote the sweep happened to think of.
_RESULT_KEYS = (trajectory_key, diagnostics_key)

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
    #: The tasks that keep the queue honest while nobody is asking anything: the
    #: reaper and the expiry sweep. Held so that liveness can say whether they
    #: are still running, which is the one thing about this process that can
    #: fail without any request failing.
    background: list[asyncio.Task[None]] = field(default_factory=list)


def _refuse(
    status: int, complaint: str, headers: dict[str, str] | None = None
) -> Response:
    """Say no in the shape everything else here says no in.

    One body for every refusal, whatever the status, because the client reads
    one shape and a service that answered a rate limit with a different document
    from a rejected configuration would need two readers for the same event.
    """
    return JSONResponse(
        status_code=status,
        content=Rejection(
            problems=[Problem(setting="service", complaint=complaint)]
        ).model_dump(),
        headers=headers,
    )


def _limits() -> Limits:
    return Limits(
        max_particles=limits.MAX_PARTICLES,
        max_steps=limits.MAX_STEPS,
        max_work=limits.MAX_WORK,
        max_queue=limits.MAX_QUEUE,
        solvers=list(limits.ALLOWED_SOLVERS),
    )


def _refusal(alive: int, queued: int, spent: float) -> str:
    """Why the service is not taking runs, or empty when it is.

    A clause rather than a sentence, because it is read in two places and each
    puts it in a sentence of its own: the capabilities endpoint sends it so the
    console can draw its controls back with the reason attached, and a
    submission that arrives anyway is refused with the same words. One statement
    of the rule rather than two that drift.

    Order matters. A service with no worker and a full queue has a queue that is
    full because there is no worker, so the worker is what is worth saying.
    """
    if alive == 0:
        return "no worker is available to take a run"
    if queued >= limits.MAX_QUEUE:
        return f"the queue is full at {queued} runs"
    if spent >= limits.BUDGET:
        hours = round(limits.BUDGET_WINDOW.total_seconds() / 3600)
        return (
            f"it has taken on as much computing as it allows itself in {hours} "
            f"hours, and starts taking runs again as the earlier ones fall out "
            f"of that window"
        )
    return ""


async def _capabilities(service: Service) -> Capabilities:
    counts = await service.jobs.counts()
    alive, version = await service.jobs.workers(
        visibility=service.settings.visibility_timeout
    )
    measured = await service.jobs.reference()
    spent = await service.jobs.spent(window=limits.BUDGET_WINDOW)
    refusal = _refusal(alive, counts.queued, spent)

    return Capabilities(
        # What a worker reported, where one has. This package's own version is
        # the fallback and is the less useful answer: what matters to somebody
        # reading a trajectory is the simulator that produced it.
        version=version or __version__,
        limits=_limits(),
        # Every condition, because they are different failures with the same
        # answer for the client: there is nothing to take a run now. ADR-0054.
        accepting=refusal == "",
        refusal=refusal,
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


#: How often results old enough to remove are looked for.
#:
#: Hourly. The retention is measured in days, so a sweep that runs every hour is
#: exact to well within the promise, and it is cheap: one indexed query that
#: finds nothing on almost every pass.
SWEEP_SECONDS = 3600


async def _sweep_once(service: Service) -> int:
    """Remove one batch of expired results. Returns how many jobs it cleared.

    The objects go first and the rows are marked after. That order is the one
    that survives being interrupted: a sweep that died in the middle has deleted
    objects whose rows still name them, and the next pass finds the same rows
    and deletes objects that are already gone, which the store treats as a
    success. The other order would leave rows saying the result is expired while
    the objects sat in the bucket for ever, which is the thing this exists to
    prevent.
    """
    identifiers = await service.jobs.stale(after=limits.RETENTION)
    if not identifiers:
        return 0

    keys = [key(identifier) for identifier in identifiers for key in _RESULT_KEYS]
    await asyncio.to_thread(service.storage.delete, keys)
    await service.jobs.forget(identifiers)
    return len(identifiers)


async def _sweep(service: Service) -> None:
    """Expire results for as long as the process is up."""
    while True:
        try:
            await asyncio.sleep(SWEEP_SECONDS)
            cleared = await _sweep_once(service)
            if cleared:
                logger.info("expired the results of %d run(s)", cleared)
        except asyncio.CancelledError:
            raise
        except Exception:
            # A store that is briefly unreachable is not a reason to stop
            # expiring results for the life of the process.
            logger.exception("the expiry sweep failed; trying again")


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
            await asyncio.to_thread(storage.ensure_lifecycle, limits.RETENTION)

            if not configured.address_salt:
                logger.warning(
                    "ORRERY_ADDRESS_SALT is not set, so the rate limit's "
                    "hashes are of the addresses alone"
                )

            service = Service(
                settings=configured,
                jobs=jobs,
                storage=storage,
                progress=Progress(configured.database_url, jobs),
            )
            await service.progress.start()
            background = [
                asyncio.create_task(_reap(service), name="reaper"),
                asyncio.create_task(_sweep(service), name="sweeper"),
            ]
            service.background = background

            app.state.service = service
            try:
                yield
            finally:
                for task in background:
                    task.cancel()
                for task in background:
                    with contextlib.suppress(asyncio.CancelledError):
                        await task
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

    @app.middleware("http")
    async def bound_the_body(request: Request, call_next):  # type: ignore[no-untyped-def]
        """Refuse a body too large to be a configuration, before reading it.

        The configuration ceiling in `limits` is the one with the helpful
        message, and it is applied to a string that has already been read into
        memory and parsed as JSON. This is the one that stops a megabyte getting
        that far, and it is deliberately blunt: the length the sender declared,
        against a ceiling four times the configuration's, before any of the body
        is touched.

        A body whose length is not declared is refused as well. Everything that
        legitimately submits to this service sends a string with a known length,
        and accepting a chunked upload of unknown size would leave the ceiling
        unenforceable at exactly the moment it matters.
        """
        if request.method == "POST":
            declared = request.headers.get("content-length")
            if declared is None or not declared.isdigit():
                return _refuse(411, "reads bodies whose length is declared")
            if int(declared) > limits.MAX_BODY_BYTES:
                return _refuse(
                    413,
                    f"reads at most {limits.MAX_BODY_BYTES} bytes in a request, "
                    f"and a configuration is a few hundred",
                )
        return await call_next(request)

    @app.middleware("http")
    async def record_the_request(request: Request, call_next):  # type: ignore[no-untyped-def]
        """Give the request an identifier, then say what happened to it.

        The identifier is the client's if it sent one and a fresh one otherwise,
        and it goes back in the response header either way, so that somebody
        looking at a slow request in a browser has the value to search the logs
        for. Everything logged while this request is handled carries it without
        being passed it.

        The route is counted by its template rather than by its path. There is
        one `/jobs/{identifier}` and there are as many paths as there have ever
        been jobs, and a counter labelled with the second is a metrics endpoint
        that grows for ever.
        """
        supplied = clean_request_id(request.headers.get("x-request-id", ""))
        identifier = supplied or uuid.uuid4().hex
        started = time.monotonic()

        with context(request=identifier):
            response = await call_next(request)
            route = request.scope.get("route")
            template = getattr(route, "path", request.url.path)
            elapsed = (time.monotonic() - started) * 1000

            count(
                "orrery_requests_total",
                route=template,
                status=str(response.status_code),
            )
            logger.info(
                "%s %s %d",
                request.method,
                template,
                response.status_code,
                extra={
                    "method": request.method,
                    "route": template,
                    "status": response.status_code,
                    "ms": round(elapsed, 1),
                },
            )
            response.headers["x-request-id"] = identifier
            return response

    def service_of(request: Request) -> Service:
        return request.app.state.service

    @app.get("/capabilities", response_model=Capabilities)
    async def capabilities(request: Request) -> Capabilities:
        return await _capabilities(service_of(request))

    @app.get("/health", response_model=Health)
    async def health(request: Request) -> Response:
        """Whether this process is working, and nothing about anything else.

        Deliberately touches neither the database nor the store. This is the
        answer an orchestrator restarts a container over, and a probe that
        failed when the database was briefly unreachable would restart every API
        container in the deployment at the moment the database could least
        afford the reconnections. What cannot fail any other way is the
        background: a reaper that has died leaves jobs claimed by machines that
        no longer exist, and no request would ever notice.
        """
        service = service_of(request)
        stopped = [task.get_name() for task in service.background if task.done()] + (
            [] if service.progress.running else ["progress"]
        )
        if stopped:
            return JSONResponse(
                status_code=503,
                content=Health(
                    alive=True,
                    ready=False,
                    detail=f"stopped running: {', '.join(stopped)}",
                ).model_dump(),
            )
        return JSONResponse(
            content=Health(alive=True, ready=True, detail="").model_dump()
        )

    @app.get("/ready", response_model=Health)
    async def ready(request: Request) -> Response:
        """Whether this process can do the work, which needs both dependencies.

        The database and the object store, because a submission writes to one
        and every result is read from the other, and a process that can reach
        neither should be taken out of the rotation rather than restarted. Both
        are checked rather than the first: an API that could queue runs and not
        serve their results would pass a database-only probe.
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
        try:
            await asyncio.to_thread(service.storage.ensure_bucket)
        except StorageError as error:
            return JSONResponse(
                status_code=503,
                content=Health(
                    alive=True, ready=False, detail=f"the object store: {error}"
                ).model_dump(),
            )
        return JSONResponse(
            content=Health(alive=True, ready=True, detail="").model_dump()
        )

    @app.get("/metrics")
    async def metrics(request: Request) -> Response:
        """What this process has done, and what the queue looks like now.

        The gauges are read here rather than kept, because they are properties
        of the database: two API containers each reporting their own count of a
        shared queue would be two answers to a question with one.
        """
        service = service_of(request)
        counts = await service.jobs.counts()
        alive, _ = await service.jobs.workers(
            visibility=service.settings.visibility_timeout
        )
        spent = await service.jobs.spent(window=limits.BUDGET_WINDOW)

        # The empty label means a series with no labels at all, which is what
        # the three single-valued gauges want.
        gauges = {
            "orrery_jobs": {"queued": counts.queued, "running": counts.running},
            "orrery_workers": {"": alive},
            "orrery_budget_used": {"": spent},
            "orrery_budget_total": {"": limits.BUDGET},
        }
        return Response(
            content=render(gauges),
            media_type="text/plain; version=0.0.4; charset=utf-8",
        )

    @app.post("/jobs", response_model=Accepted)
    async def submit(request: Request, submission: Submission) -> Response:
        service = service_of(request)

        configuration, problems = check_submission(submission.configuration)
        if configuration is None:
            # 422 rather than 400: the request is well formed and its content is
            # a document this service will not run, which is what that code is
            # for. The body is every objection at once.
            count("orrery_submissions_total", outcome="rejected")
            return JSONResponse(
                status_code=422, content=Rejection(problems=problems).model_dump()
            )

        capability = await _capabilities(service)
        if not capability.accepting:
            # Refused rather than queued for later. A job accepted into a queue
            # nothing is draining is worse than a refusal, because the person
            # who submitted it waits instead of being told to look at the
            # gallery. ADR-0054.
            count("orrery_submissions_total", outcome="refused")
            return _refuse(
                503,
                f"is not taking runs at the moment: {capability.refusal}. The "
                f"published runs in the gallery are unaffected",
            )

        # After the ceilings on the run and before anything is stored, because
        # the answer to somebody submitting their seventh run in an hour is the
        # same whatever the run was, and telling them so should not cost a write.
        submitter = submitter_of(
            address_of(
                request, trust_forwarded_for=service.settings.trust_forwarded_for
            ),
            service.settings.address_salt,
        )
        taken = await service.jobs.submissions(submitter, window=limits.WINDOW)
        if taken >= limits.MAX_SUBMISSIONS_PER_ADDRESS:
            minutes = round(limits.WINDOW.total_seconds() / 60)
            count("orrery_submissions_total", outcome="limited")
            return _refuse(
                429,
                f"takes at most {limits.MAX_SUBMISSIONS_PER_ADDRESS} runs from "
                f"one address every {minutes} minutes, and this address has "
                f"submitted {taken}. The runs already submitted are unaffected, "
                f"as are the published ones in the gallery",
                headers={"Retry-After": str(round(limits.WINDOW.total_seconds()))},
            )

        settled = plan(configuration)
        job, duplicate = await service.jobs.submit(
            settled,
            submitter=submitter,
            # Carried on to the worker, so that this line and the run's are one
            # story rather than two.
            request_id=REQUEST_ID.get(),
            work=limits.work_units(
                settled.particles, settled.steps, configuration.solver.kind
            ),
        )
        count(
            "orrery_submissions_total",
            outcome="duplicate" if duplicate else "queued",
        )
        logger.info(
            "queued %s",
            job.id,
            extra={
                "job": job.id,
                "particles": job.particles,
                "steps": job.steps,
                "duplicate": duplicate,
            },
        )
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

    async def _serve(
        service: Service, key: str, request: Request, media_type: str, name: str
    ) -> Response:
        """A stored object, answering a range request over it.

        Ranged because that is how the trajectory reader fetches: it starts
        playing a run before the whole file has arrived, and it does that by
        asking for the header and then for frames. A server that ignored the
        range would be correct and would cost the reader the download it exists
        to avoid.

        boto3 is synchronous and has no async client, which is fine and has to
        be handled rather than ignored. The length is asked for in a thread,
        because a blocking network call on the event loop would stall every
        other request for as long as the store took to answer. The body is a
        synchronous iterator, which Starlette runs in a thread for the same
        reason without being asked.
        """
        length = await asyncio.to_thread(service.storage.size, key)
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

    def _missing(gone: str, never: str, expired: bool) -> Response:
        """Why there is nothing to fetch, which is two different things.

        A result that was here and has been removed is worth saying plainly,
        with how long results are kept, because the person asking has a link
        that used to work and the alternative is for them to conclude the
        service loses things.
        """
        days = round(limits.RETENTION.total_seconds() / 86_400)
        return JSONResponse(
            status_code=404,
            content={
                "detail": (
                    f"{gone}: results are kept for {days} days" if expired else never
                )
            },
        )

    @app.get("/jobs/{identifier}/trajectory")
    async def trajectory(request: Request, identifier: str) -> Response:
        service = service_of(request)
        result = await service.jobs.keys(identifier)
        if result.trajectory is None:
            return _missing(
                "this run's trajectory has been removed",
                "this job has not produced a trajectory",
                result.expired,
            )
        return await _serve(
            service,
            result.trajectory,
            request,
            "application/octet-stream",
            "trajectory",
        )

    @app.get("/jobs/{identifier}/diagnostics")
    async def diagnostics(request: Request, identifier: str) -> Response:
        service = service_of(request)
        result = await service.jobs.keys(identifier)
        if result.diagnostics is None:
            return _missing(
                "this run's diagnostics have been removed",
                "this job has not produced any diagnostics",
                result.expired,
            )
        return await _serve(
            service, result.diagnostics, request, "text/csv", "diagnostics"
        )

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
    """Run the API under uvicorn, for a container's entry point.

    The server is started inside an event loop this process makes rather than
    through `uvicorn.run`, which makes its own and chooses the kind. That choice
    is the wrong one on Windows, where it takes the proactor loop and psycopg's
    connections then never answer, so every request to a service that came up
    apparently healthy times out. Asking uvicorn for the plain asyncio loop and
    starting it here is what makes the two agree.
    """
    import uvicorn

    configure_logging()
    use_compatible_event_loop()

    server = uvicorn.Server(
        uvicorn.Config(
            create_app(),
            # Every interface, because the process is addressed from outside the
            # container it runs in.
            host="0.0.0.0",
            port=8000,
            loop="asyncio",
            # uvicorn's own logging configuration is declined, so that its
            # records go through the handler configured above and come out as
            # JSON like everything else. Its access log is off because the
            # middleware already writes one line a request, with the route
            # template rather than the path and with the identifier attached.
            log_config=None,
            access_log=False,
        )
    )
    asyncio.run(server.serve())


if __name__ == "__main__":
    main()
