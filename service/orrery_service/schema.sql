-- The whole of the service's state.
--
-- Two tables and a trigger. There is no migration tool and no object-relational
-- mapper, because this is shorter than the configuration either would need, and
-- because the queue below is written in SQL that an mapper would have to be
-- talked out of rewriting. ADR-0056 is the argument for the queue living here at
-- all rather than in a broker beside the database.
--
-- Applied on start-up by every process, which is why every statement is written
-- to be safe to run again. A service that needed a separate migration step to be
-- deployed would have a state where the image is new and the schema is not.

CREATE TABLE IF NOT EXISTS job (
    id uuid PRIMARY KEY,

    -- The SHA-256 of the settled configuration, which is what makes a job
    -- idempotent. Unique, so the same run submitted twice is one row and the
    -- second submission is answered with the first result.
    content_hash text NOT NULL UNIQUE,

    -- The configuration the worker will run, output section included. Stored
    -- rather than reconstructed, so that a job carries the document it ran and
    -- a change to the service's planning does not retrospectively change what
    -- an old job says it did.
    configuration text NOT NULL,

    state text NOT NULL CHECK (state IN ('queued', 'running', 'done', 'failed')),

    -- How many times it has been claimed. Above one means a worker died on it.
    attempts integer NOT NULL DEFAULT 0,

    -- What the run is, decided when it was accepted so that a client can be
    -- told how many frames to expect before anything has started.
    particles bigint NOT NULL,
    steps bigint NOT NULL,
    timestep double precision NOT NULL,
    stride bigint NOT NULL,
    frames bigint NOT NULL,

    created_at timestamptz NOT NULL DEFAULT now(),
    claimed_at timestamptz,

    -- The last time the worker holding it said so. The visibility timeout is
    -- measured from here, so a worker that has stopped answering loses the job
    -- without anything having to notice that it died.
    heartbeat_at timestamptz,
    finished_at timestamptz,

    worker text NOT NULL DEFAULT '',

    -- Progress, as the worker measures it from the run's own output.
    progress_step bigint NOT NULL DEFAULT 0,
    progress_time double precision NOT NULL DEFAULT 0,
    step_ms double precision,
    energy_drift double precision,

    -- Where the result went. Object storage keys rather than URLs, because a
    -- URL is a property of how the service is reached and would be wrong the
    -- first time the service moved. ADR-0057.
    trajectory_key text,
    diagnostics_key text,
    trajectory_bytes bigint,

    error text NOT NULL DEFAULT ''
);

-- The index the claim reads: the oldest queued job, without walking the
-- finished ones. Partial, because the finished rows are the great majority
-- after a day and none of them is ever a candidate.
CREATE INDEX IF NOT EXISTS job_queued ON job (created_at) WHERE state = 'queued';

-- The one the reaper reads, for the same reason.
CREATE INDEX IF NOT EXISTS job_running ON job (heartbeat_at) WHERE state = 'running';

-- Which workers are alive.
--
-- Separate from the jobs, because the question the client asks is whether
-- anything is there to take a run, and a worker sitting idle holds no job to be
-- asked about. Without this the service could not tell an empty queue from a
-- dead one, and telling those apart is the whole of ADR-0054.
CREATE TABLE IF NOT EXISTS worker (
    name text PRIMARY KEY,
    heartbeat_at timestamptz NOT NULL DEFAULT now(),
    -- What the worker's binary answers to --version, so the API can report the
    -- simulator that will actually run rather than the one this package was
    -- released beside.
    version text NOT NULL DEFAULT ''
);

-- Tell the listeners that a job changed.
--
-- A trigger rather than a NOTIFY beside each statement, so that a code path
-- added later cannot forget one. The payload is the identifier alone: a
-- listener re-reads the row, which means it always sends the current state
-- rather than a queue of stale ones, and means the payload cannot outgrow what
-- NOTIFY carries.
CREATE OR REPLACE FUNCTION job_changed() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('orrery_job', NEW.id::text);
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS job_notify ON job;
CREATE TRIGGER job_notify
    AFTER INSERT OR UPDATE ON job
    FOR EACH ROW EXECUTE FUNCTION job_changed();
