"""The HTTP and WebSocket surface, against a real database.

Driven through Starlette's test client rather than through an async client,
because the client runs the application's lifespan and opens WebSockets, and
this suite needs both: the pool, the progress listener and the reaper are all
started by the lifespan, and half of what is worth testing here is the socket.
"""

from __future__ import annotations

from collections.abc import Iterator

import psycopg
import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from orrery_service.api import create_app
from orrery_service.limits import (
    BUDGET,
    MAX_BODY_BYTES,
    MAX_PARTICLES,
    MAX_SUBMISSIONS_PER_ADDRESS,
    WINDOW,
)
from orrery_service.observability import reset

from .conftest import (
    DATABASE_URL,
    needs_database,
    needs_storage,
    reset_database,
    settings_for_tests,
)

pytestmark = [pytest.mark.integration, needs_database, needs_storage]

CLUSTER = """
[run]
timestep = 0.001
steps = 1000
seed = 7

[initial_conditions]
kind = plummer
count = 64
"""


def pretend_a_worker_is_alive(name: str = "worker-1") -> None:
    """Put a live worker in the table without starting one.

    What most of these cases need is a service that is accepting runs, and
    whether anything then takes them is a different test. `test_worker.py` is
    where a real one runs.
    """
    with psycopg.connect(DATABASE_URL) as connection:
        connection.execute(
            "INSERT INTO worker (name, heartbeat_at, version) "
            "VALUES (%s, now(), %s) "
            "ON CONFLICT (name) DO UPDATE SET heartbeat_at = now()",
            (name, "orrery 1.0.0, double precision"),
        )
        connection.commit()


@pytest.fixture
def client() -> Iterator[TestClient]:
    reset_database()
    # The counters live in the process rather than in the database, so a case
    # that asserts one would otherwise be reading what the case before it did.
    reset()
    with TestClient(create_app(settings_for_tests())) as client:
        yield client


def test_health_reports_alive_and_ready(client: TestClient) -> None:
    answer = client.get("/health")
    assert answer.status_code == 200
    assert answer.json() == {"alive": True, "ready": True, "detail": ""}


def test_readiness_is_a_different_question_from_liveness(client: TestClient) -> None:
    """One says restart me, the other says do not send me traffic.

    Liveness touches neither dependency on purpose: a probe that failed when the
    database blinked would restart every API container at the moment the
    database could least afford the reconnections. What it does check is the
    background work nothing else would notice the loss of.
    """
    assert client.get("/health").json() == {"alive": True, "ready": True, "detail": ""}

    ready = client.get("/ready")
    assert ready.status_code == 200
    assert ready.json()["ready"] is True


def test_liveness_fails_when_the_background_has_stopped(client: TestClient) -> None:
    service = client.app.state.service
    service.background[0].cancel()

    answer = client.get("/health")
    assert answer.status_code == 503
    assert "reaper" in answer.json()["detail"]


def test_every_answer_carries_the_request_it_belongs_to(client: TestClient) -> None:
    given = client.get("/capabilities", headers={"X-Request-Id": "abc-123"})
    assert given.headers["x-request-id"] == "abc-123"

    # And one is made up when the client did not send one, so that every line
    # in the log has something to be searched by.
    made = client.get("/capabilities")
    assert made.headers["x-request-id"] != ""

    # A header a stranger controls cannot put a newline into a log line.
    filtered = client.get("/capabilities", headers={"X-Request-Id": "a\tb c"})
    assert filtered.headers["x-request-id"] == "abc"


def test_metrics_report_the_queue_and_what_this_process_has_answered(
    client: TestClient,
) -> None:
    pretend_a_worker_is_alive()
    client.post("/jobs", json={"configuration": CLUSTER})

    written = client.get("/metrics").text
    assert 'orrery_submissions_total{outcome="queued"} 1' in written
    assert 'orrery_jobs{state="queued"} 1' in written
    assert "orrery_workers 1" in written
    # The route rather than the path, so that a metric does not grow a series
    # for every job ever submitted.
    assert 'route="/jobs/{identifier}"' not in written
    assert 'orrery_requests_total{route="/jobs",status="202"} 1' in written


def test_capabilities_states_the_ceilings(client: TestClient) -> None:
    body = client.get("/capabilities").json()
    assert body["limits"]["max_particles"] == MAX_PARTICLES
    assert body["limits"]["solvers"] == ["direct", "barnes-hut"]
    # Nothing has run, so there is no measured rate and the client is told so
    # rather than shown a figure nothing measured.
    assert body["reference"] is None


def test_nothing_is_accepted_while_no_worker_is_alive(client: TestClient) -> None:
    assert client.get("/capabilities").json()["accepting"] is False

    answer = client.post("/jobs", json={"configuration": CLUSTER})
    assert answer.status_code == 503
    problems = answer.json()["problems"]
    assert problems[0]["setting"] == "service"
    # And it says where to go instead, which is the whole of the degradation.
    assert "gallery" in problems[0]["complaint"]


def test_a_submission_is_accepted_and_queued(client: TestClient) -> None:
    pretend_a_worker_is_alive()
    assert client.get("/capabilities").json()["accepting"] is True

    answer = client.post("/jobs", json={"configuration": CLUSTER})
    assert answer.status_code == 202
    body = answer.json()
    assert body["duplicate"] is False

    job = body["job"]
    assert job["state"] == "queued"
    assert job["position"] == 0
    assert job["particles"] == 64
    assert job["steps"] == 1000
    assert job["frames"] > 1
    assert job["trajectory"] is None


def test_the_same_run_twice_returns_the_first_job(client: TestClient) -> None:
    pretend_a_worker_is_alive()
    first = client.post("/jobs", json={"configuration": CLUSTER}).json()
    answer = client.post("/jobs", json={"configuration": CLUSTER})

    assert answer.status_code == 200
    body = answer.json()
    assert body["duplicate"] is True
    assert body["job"]["id"] == first["job"]["id"]


def test_a_configuration_that_is_not_a_run_is_refused_with_its_reasons(
    client: TestClient,
) -> None:
    pretend_a_worker_is_alive()
    answer = client.post(
        "/jobs",
        json={"configuration": "[run]\nsteps = 0\n[initial_conditions]\ncount = 1\n"},
    )
    assert answer.status_code == 422
    named = [problem["setting"] for problem in answer.json()["problems"]]
    # Every objection at once rather than the first.
    assert named == ["run.timestep", "run.steps", "initial_conditions.count"]


def test_a_ceiling_is_reported_as_a_refusal(client: TestClient) -> None:
    pretend_a_worker_is_alive()
    answer = client.post(
        "/jobs",
        json={"configuration": CLUSTER.replace("count = 64", "count = 400000")},
    )
    assert answer.status_code == 422
    problems = answer.json()["problems"]
    assert problems[0]["setting"] == "initial_conditions.count"
    assert str(MAX_PARTICLES) in problems[0]["complaint"]


def test_one_address_is_held_to_a_rate(client: TestClient) -> None:
    """The ceiling that stops one visitor occupying the worker all afternoon.

    Each submission is a different run, because the same one twice is one job
    and would not be charged twice. What is asserted beyond the status is that
    the refusal says how long to wait, so a client has something to act on
    rather than a number.
    """
    pretend_a_worker_is_alive()
    for seed in range(MAX_SUBMISSIONS_PER_ADDRESS):
        answer = client.post(
            "/jobs",
            json={"configuration": CLUSTER.replace("seed = 7", f"seed = {seed}")},
        )
        assert answer.status_code == 202, answer.text

    answer = client.post(
        "/jobs", json={"configuration": CLUSTER.replace("seed = 7", "seed = 99")}
    )
    assert answer.status_code == 429
    assert answer.headers["retry-after"] == str(round(WINDOW.total_seconds()))
    complaint = answer.json()["problems"][0]["complaint"]
    assert str(MAX_SUBMISSIONS_PER_ADDRESS) in complaint
    # And it says what is unaffected, which is the whole gallery.
    assert "gallery" in complaint


def test_the_budget_stops_the_service_taking_more_work(client: TestClient) -> None:
    """A day's worth of computing, whoever asked for it.

    Written by putting the work in the table rather than by submitting until the
    budget is gone, which at one full-size run apiece would be twenty-four runs
    and rather more of the suite's time than the property needs.
    """
    pretend_a_worker_is_alive()
    with psycopg.connect(DATABASE_URL) as connection:
        connection.execute(
            "INSERT INTO job (id, content_hash, configuration, state, particles, "
            "steps, timestep, stride, frames, work) VALUES "
            "(gen_random_uuid(), 'spent', '', 'done', 2, 1, 0.1, 1, 2, %s)",
            (BUDGET,),
        )
        connection.commit()

    capabilities = client.get("/capabilities").json()
    assert capabilities["accepting"] is False
    assert "computing" in capabilities["refusal"]

    answer = client.post("/jobs", json={"configuration": CLUSTER})
    assert answer.status_code == 503
    assert "gallery" in answer.json()["problems"][0]["complaint"]


def test_a_body_larger_than_a_configuration_is_refused_unread(
    client: TestClient,
) -> None:
    pretend_a_worker_is_alive()
    answer = client.post("/jobs", json={"configuration": "#" * (MAX_BODY_BYTES + 1)})
    assert answer.status_code == 413
    assert answer.json()["problems"][0]["setting"] == "service"


def test_a_range_header_cannot_ask_for_arbitrary_arithmetic(
    client: TestClient,
) -> None:
    """A header is a stranger's input, and this one is turned into integers.

    Answered as a request with no valid range rather than as an error: the
    pattern does not match, so the whole object is what was asked for, and the
    job has no result to serve anyway.
    """
    pretend_a_worker_is_alive()
    job = client.post("/jobs", json={"configuration": CLUSTER}).json()["job"]
    answer = client.get(
        f"/jobs/{job['id']}/trajectory", headers={"Range": f"bytes={'9' * 100000}-"}
    )
    assert answer.status_code == 404


def test_an_unknown_job_is_a_not_found(client: TestClient) -> None:
    assert client.get("/jobs/d3adbeef-0000-0000-0000-000000000000").status_code == 404
    assert client.get("/jobs/not-a-uuid").status_code == 404


def test_a_job_that_has_not_run_has_no_result_to_fetch(client: TestClient) -> None:
    pretend_a_worker_is_alive()
    job = client.post("/jobs", json={"configuration": CLUSTER}).json()["job"]
    assert client.get(f"/jobs/{job['id']}/trajectory").status_code == 404
    assert client.get(f"/jobs/{job['id']}/diagnostics").status_code == 404


def test_the_socket_sends_the_job_as_soon_as_it_opens(client: TestClient) -> None:
    """A queued job does not change for minutes, so it is sent at once.

    Without this a client could not tell a working socket from a broken one
    until the run it is waiting for started.
    """
    pretend_a_worker_is_alive()
    job = client.post("/jobs", json={"configuration": CLUSTER}).json()["job"]

    with client.websocket_connect(f"/jobs/{job['id']}/progress") as socket:
        first = socket.receive_json()
    assert first["id"] == job["id"]
    assert first["state"] == "queued"


def test_the_socket_pushes_a_change(client: TestClient) -> None:
    pretend_a_worker_is_alive()
    job = client.post("/jobs", json={"configuration": CLUSTER}).json()["job"]

    with client.websocket_connect(f"/jobs/{job['id']}/progress") as socket:
        assert socket.receive_json()["state"] == "queued"

        # What a worker claiming it and reporting progress does to the row. The
        # change is announced by the trigger, so nothing here tells the socket.
        with psycopg.connect(DATABASE_URL) as connection:
            connection.execute(
                "UPDATE job SET state = 'running', worker = 'worker-1', "
                "attempts = 1, claimed_at = now(), heartbeat_at = now(), "
                "progress_step = 250, progress_time = 0.25 WHERE id = %s",
                (job["id"],),
            )
            connection.commit()

        update = socket.receive_json()

    assert update["state"] == "running"
    assert update["progress"]["step"] == 250
    assert update["position"] is None


def test_the_socket_refuses_a_job_that_does_not_exist(client: TestClient) -> None:
    # Accepted and then closed, rather than refused at the handshake. A browser
    # is told nothing useful about a rejected handshake, and the close code is
    # something a client can act on.
    with (
        client.websocket_connect(
            "/jobs/d3adbeef-0000-0000-0000-000000000000/progress"
        ) as socket,
        pytest.raises(WebSocketDisconnect) as closed,
    ):
        socket.receive_json()
    assert closed.value.code == 4404
