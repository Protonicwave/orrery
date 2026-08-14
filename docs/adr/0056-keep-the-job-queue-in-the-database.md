# ADR-0056: Keep the job queue in the database that already holds the jobs

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

A submitted run has to wait somewhere. There is one worker, a run takes a couple
of minutes, and several people may press the button at once, so something has to
hold the waiting jobs, hand each to exactly one worker, and cope with a worker
that dies holding one.

The reflex answer is a broker: Redis with a list, or a queue service. The usual
argument for it is that a database is not a queue, which was true when the
alternative was polling a table with a lock nobody could hold safely, and stopped
being true in PostgreSQL 9.5.

Against that, the jobs already have to be in a database. A job has a state, a
configuration, a progress report, a result and a content hash, and the client
asks about all of it by identifier. So the choice is not between a database and a
broker. It is between one system and two.

## Decision

The queue is a table in PostgreSQL, and a worker claims a job with one statement:

```sql
WITH taken AS (
    SELECT id FROM job
    WHERE state = 'queued' AND attempts < %s
    ORDER BY created_at
    FOR UPDATE SKIP LOCKED
    LIMIT 1
)
UPDATE job SET state = 'running', attempts = job.attempts + 1, ...
FROM taken WHERE job.id = taken.id
RETURNING ...
```

`FOR UPDATE SKIP LOCKED` is the whole mechanism. Each worker locks one row and
steps over rows another worker has already locked, so two workers running this at
the same instant take two different jobs, or one takes nothing, and neither
outcome can happen half way.

The `LIMIT 1` is inside the locking select rather than outside it. Outside, both
workers would select the same row and one would wait for the other rather than
skipping past it, which is the difference between a queue and a line of workers
taking turns to be blocked.

## What a broker would have provided, and where it is instead

A queue is not the hard part. The hard part is what happens when a worker
disappears while holding a job, and a broker earns its keep by having answers.
Each answer here is a few lines of SQL, and all of them are in
`service/orrery_service/queue.py`.

**A visibility timeout.** A claim is only good while the worker keeps saying so.
The worker beats every fifteen seconds, and `reap` returns any running job whose
last beat was more than ninety seconds ago. That is what makes killing a worker
safe rather than leaving a row that says a run is in progress on a machine that
no longer exists.

**A retry limit.** The claim increments an attempt count, so a job that has
killed three workers is failed with a message saying so instead of being tried
for ever. A job that has killed two workers is more likely to be a job that kills
workers than one that met two unlucky machines.

**Fencing.** A reaped worker that has not noticed must not be able to finish the
job somebody else is now running. Every write a worker makes is conditional on
the row still naming it, so the beat that fails is how it finds out, and it kills
the run rather than uploading a second trajectory over the winner's.

**Notification.** The client watches a run over a WebSocket, and the API learns
that a job changed from a trigger that calls `pg_notify` on every insert and
update. A trigger rather than a `NOTIFY` beside each statement, so that a code
path added later cannot forget one.

## Alternatives considered

**Redis, with a list and a reliable-queue pattern.** A second service to run, to
back up and to reason about, plus a second place the truth about a job lives, and
therefore the question of what happens when the two disagree. At one worker and a
queue that is empty most of the time, it buys nothing this does not have.

**A hosted queue service.** Same objection, plus a bill and a dependency on a
provider for the one part of the system that is otherwise a container and a
database.

**`LISTEN` alone, with no polling by the worker.** The worker could wait to be
told a job arrived rather than asking every two seconds. It would save a query
every two seconds, which is nothing, and it would mean a worker that missed a
notification while reconnecting sat idle beside a full queue. Asking is the
behaviour that recovers by itself.

**Advisory locks.** They would work and they are less legible: the lock is held
in a session rather than visible in the row, so a job's state and the fact that
somebody holds it live in two different places.

## Consequences

There is no scheduler, no priority and no fairness beyond first come first
served. `ORDER BY created_at` is the policy, and with no accounts there is
nothing to be fair between.

The reaper runs in the API rather than in the worker, because a worker that has
died cannot return its own job. That makes the API the process that must be up
for the queue to recover, which it has to be anyway for anybody to submit
anything.

The concurrency claim is tested rather than asserted.
`service/tests/test_queue.py` runs twenty workers at ten jobs and requires the
ten claims to be distinct, kills a worker part way through a run and requires the
job to come back and be finished by another, and requires a reaped worker's
attempt to finish a job to be refused. Those cases need a live PostgreSQL and
skip themselves without one, because nothing that stands in for a database
reproduces what `SKIP LOCKED` does.
