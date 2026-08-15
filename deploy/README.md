# Deploying the service

Four containers on one machine: PostgreSQL, an S3-compatible object store, the
API and one worker. There is no orchestrator, because one container per role is
the right size for a service that runs one simulation at a time and the
alternative is a cluster to administer for the benefit of a single process.

| File | What it is |
| --- | --- |
| `compose.yaml` | The stack. Used by the tests, by continuous integration and, with the overlay below, in front of the public |
| `production.yaml` | The four differences a deployment makes: durable data, nothing published but the API, credentials from outside the repository, and restart on failure |
| `api.Dockerfile` | Python and the service. No simulator, because the API never runs one |
| `worker.Dockerfile` | The simulator, built from this checkout, and the Python that feeds it |
| `backup.sh`, `restore.sh` | The metadata database, out and back in |

## On a laptop

```
docker compose -f deploy/compose.yaml up --build
```

The API is on port 8000, the store on 9000 and PostgreSQL on 5432, and the
credentials are in the file because that stack is for a laptop and for a
continuous integration runner. Nothing in it is a deployment.

## On a host

Write `deploy/production.env`. It is never committed, and `.gitignore` has the
pattern that keeps it out:

```
ORRERY_POSTGRES_USER=orrery
ORRERY_POSTGRES_PASSWORD=<generated>
ORRERY_POSTGRES_DB=orrery
ORRERY_DATABASE_URL=postgresql://orrery:<generated>@postgres:5432/orrery
ORRERY_STORAGE_ACCESS_KEY=<generated>
ORRERY_STORAGE_SECRET_KEY=<generated>
ORRERY_ALLOWED_ORIGINS=https://<the published site>
ORRERY_ADDRESS_SALT=<generated>
```

Then:

```
docker compose --env-file deploy/production.env \
  -f deploy/compose.yaml -f deploy/production.yaml up -d --build
```

The API listens on `127.0.0.1:8000`. Something on the host terminates TLS and
forwards to it: that is the only part of this deployment the repository does not
describe, because it is the part that depends on the host. Whatever it is has to
set `X-Forwarded-For`, since the overlay tells the API to believe it, and that
is only safe because nothing else can reach the port.

Compose 2.24 or newer, for the `!override` and `!reset` tags the overlay uses to
replace lists rather than merge with them.

## What the containers are allowed to do

Both images run as an unprivileged user with a read-only root filesystem, every
capability dropped and no way to gain new privileges. The API has a small tmpfs
for the temporary directory Python expects; the worker has a volume at `/work`,
which is where a run's configuration and output are written, and which is a
volume rather than a tmpfs because a trajectory is tens of megabytes and putting
it in memory would count against the container's limit.

The worker is on the internal network and on nothing else. It reaches the
database and the object store and it has no route anywhere further: not to the
internet, not to a cloud metadata service, not to another container's port. It
is the process that runs a program over a document a stranger sent, so it is the
one that should be able to reach the least.

Neither container is given more than it needs: two cores and two gigabytes for
the worker, one core and half a gigabyte for the API, and a process limit on
both. The worker's share is also the honest figure behind what the service can
do, and it is a long way from the machine `docs/performance.md` was measured on.

## Backups

The metadata database holds the jobs, the queue and what every finished run
measured. The trajectories are not backed up: they are three orders of magnitude
larger, they expire after seven days by design, and they can be produced again
by running the configuration each job carries.

```
deploy/backup.sh backups
```

Writes `backups/orrery-<timestamp>.dump`, reads it back to check it is a dump,
and deletes ones older than a fortnight. Run it from cron, daily:

```
0 3 * * * cd /srv/orrery && deploy/backup.sh /srv/orrery/backups >> /var/log/orrery-backup.log 2>&1
```

`ORRERY_COMPOSE_FILES` overrides which compose files the two scripts address, so
that the same scripts can be run against a stack without the overlay.
Continuous integration does exactly that after the service suite, against the
database those tests have just filled, which is what stops either script rotting
between the days somebody needs one.

Restoring goes into a scratch database unless the live one is named:

```
deploy/restore.sh backups/orrery-20260815T030000Z.dump
```

which drops and recreates `orrery_restore_check`, restores into it, and prints
how many jobs came back and when the newest was submitted. That is the form to
run on an ordinary afternoon: a restore procedure nobody has taken is a hope
rather than a plan. Restoring over the live database needs it named as the
second argument and `ORRERY_RESTORE_OVER_LIVE` set, and the API and the worker
stopped first.

## What to watch

`GET /health` is liveness: it touches neither the database nor the store, and it
fails when the reaper, the expiry sweep or the progress listener has stopped.
`GET /ready` is readiness and checks both dependencies. `GET /metrics` is the
Prometheus text format: requests by route and status, submissions by outcome,
the queue's length, how many workers have beaten recently, and how much of the
day's compute budget has been spent.

None of those three needs to be reachable from the public. The proxy in front
should forward the routes the client uses and keep `/metrics` to the host: it
carries nothing secret, only aggregate counts, and it is also not something a
visitor has any use for.

Logs are one JSON object a line on standard output, each carrying the request it
belongs to. The identifier reaches the worker as well, so the line about a
submission and the lines about the run it became can be found together.
