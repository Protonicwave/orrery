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
from orrery_service.limits import MAX_PARTICLES

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
    with TestClient(create_app(settings_for_tests())) as client:
        yield client


def test_health_reports_alive_and_ready(client: TestClient) -> None:
    answer = client.get("/health")
    assert answer.status_code == 200
    assert answer.json() == {"alive": True, "ready": True, "detail": ""}


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
