# The compute service

Submit a configuration, watch it run, play the result. The service takes an
`.orrery` file, queues it, runs the native simulator over it on a machine that
is not the reader's, and hands back a trajectory the browser client plays
through the same viewer it plays the published gallery through.

It runs the binary this repository builds and does nothing to its output beyond
moving it, so a run submitted from a browser is the same run as one taken from a
command line.

## Running it

```
docker compose -f deploy/compose.yaml up --build
```

Four containers: PostgreSQL, an S3-compatible store, the API on port 8000 and
one worker. The worker's image builds the simulator from this checkout, so the
first build takes a few minutes and the rest take seconds.

Submit a run:

```
curl -X POST http://localhost:8000/jobs \
  -H 'Content-Type: application/json' \
  -d "$(python -c 'import json,sys; print(json.dumps({"configuration": open("examples/cluster.orrery").read()}))')"
```

## The routes

| Route | What it does |
| --- | --- |
| `POST /jobs` | Submit a run. 202 with the job, 200 if the same run was already submitted, 422 with every objection if it is not a run this service will take, 503 if nothing is available to take one |
| `GET /jobs/{id}` | The job as it stands, including where it is in the queue |
| `GET /jobs/{id}/trajectory` | The result, answering range requests |
| `GET /jobs/{id}/diagnostics` | The conserved quantities the run wrote |
| `WS /jobs/{id}/progress` | The same job, pushed whenever it changes |
| `GET /capabilities` | What the service will accept, and whether it is accepting |
| `GET /health` | Whether the process is alive, and whether it can work |

The contract is `orrery_service/contract.py` and nothing else. The client's half,
`web/src/service/contract.ts`, is generated from it:

```
python service/tools/export_contract.py
```

Continuous integration runs the same command with `--check` and fails if what it
writes differs from what is committed.

## What it will not do

A submission is refused rather than clamped, and the refusal names the setting.
`orrery_service/limits.py` holds the ceilings with the reason each one is where
it is; in summary, a run may ask for at most 20,000 particles and at most 20,000
steps, and at most as much work as the demonstration in the README, which is
20,000 particles over 6,000 steps. Fewer particles buys more steps.

The two SYCL solvers are refused. The worker has no GPU, and the published GPU
figures were measured on the machine `docs/performance.md` names.

The `[output]` section is refused as well. Where a run writes is the service's
business: the result is fetched from the job rather than written to a path the
submitter chose.

## Settings

Every one of these is read from the environment, and the three that name
something outside the process have no default, because a default would be a
service that comes up healthy pointed at the wrong place.

| Variable | Meaning |
| --- | --- |
| `ORRERY_DATABASE_URL` | The PostgreSQL holding the jobs and the queue |
| `ORRERY_STORAGE_ENDPOINT` | An S3-compatible endpoint |
| `ORRERY_STORAGE_BUCKET` | The bucket trajectories go in |
| `ORRERY_STORAGE_ACCESS_KEY`, `ORRERY_STORAGE_SECRET_KEY` | Its credentials |
| `ORRERY_STORAGE_REGION` | Defaults to `us-east-1`, which most stores ignore |
| `ORRERY_BINARY` | The simulator. Defaults to `orrery` on the path |
| `ORRERY_ALLOWED_ORIGINS` | Comma separated. Empty allows no browser origin |
| `ORRERY_VISIBILITY_TIMEOUT_SECONDS` | How long a claimed job may go without a heartbeat. Defaults to 90 |
| `ORRERY_HEARTBEAT_SECONDS` | How often a worker says it is alive. Defaults to 15 |
| `ORRERY_MAX_ATTEMPTS` | How many times a job may be claimed. Defaults to 3 |

## The tests

Most of the suite needs nothing:

```
cd service
pip install -e ".[test,dev]"
pytest -m "not integration"
ruff check . && ruff format --check .
```

The rest need a database, an object store and a simulator, and skip themselves
with the reason when they do not have one. To run everything:

```
docker compose -f deploy/compose.yaml up -d postgres storage
cmake --build --preset release --target orrery

ORRERY_TEST_DATABASE_URL=postgresql://orrery:orrery@localhost:5432/orrery \
ORRERY_TEST_STORAGE_ENDPOINT=http://localhost:9000 \
pytest
```

`ORRERY_TEST_DATABASE_URL` is deliberately not `ORRERY_DATABASE_URL`: the suite
empties the tables it finds, and a shell configured to run the service should
not be able to empty the service's database by running the tests in it. The
simulator is found in `build/*/apps/` unless `ORRERY_BINARY` names one.

What those tests are for is the part that cannot be reasoned about: twenty
workers claiming ten jobs at once, a worker killed part way through a run, and a
configuration going in one end as text and coming out of the other as a
trajectory the client can play.

## Where the decisions are written down

- ADR-0054, degrade to the gallery when compute is unavailable.
- ADR-0055, run the full solver server side.
- ADR-0056, keep the job queue in the database.
- ADR-0057, store trajectories outside the database.
