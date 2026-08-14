# ADR-0057: Store trajectories outside the database, and serve them through the API

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

A run produces a trajectory. At the sizes this service accepts that is a few
megabytes to a few tens of megabytes of Float32, written once by the worker and
then read by a browser, in ranges, while it is being played.

The metadata about that run is already in PostgreSQL, so the cheap thing to do
would be to put the file there too, in a `bytea` column, and have one place where
everything lives.

## Decision

Trajectories and diagnostics go into S3-compatible object storage. The database
holds the key, the length, and nothing else about the bytes.

The reasons are all about what the file is rather than about size in the
abstract:

**It is read as ranges.** `web/src/trajectory/source.ts` fetches the header,
works out the frame stride, and then fetches frames, so that a run starts playing
before the whole file has arrived. Out of a column that is a query per range,
through the connection pool, competing with the queue for the same backends. Out
of an object store it is what the store is for.

**It is written once and never updated.** A large value that is rewritten is a
problem for a database; a large value that is written once is merely a waste of
one. Neither is a reason to use a database.

**The backups have different shapes.** The metadata is small, valuable and worth
a point-in-time restore. The trajectories are large and reproducible from a
configuration and a revision of this repository, which is the property
`configuration.hpp` exists to provide. Putting them in the same backup would make
the valuable half expensive to keep.

**It has a lifecycle.** A submitted run is worth keeping for as long as somebody
might come back to the link, and not for ever. Expiry is a policy an object store
applies for you and a job somebody has to write for a table.

## The client fetches through the API, not from the store

This is the second half of the decision and the less obvious one. The API has a
route that answers a range request by copying the range out of the store.

The alternative is a presigned URL: the API hands the client a signed address and
the bytes never touch it. That is the scalable answer and it is not taken here,
for two reasons.

The first is that the browser would then be fetching from a third origin, which
needs a CORS policy on the bucket. That is a piece of configuration living in an
object store's console rather than in this repository, and it fails in a way that
looks like the trajectory reader being broken.

The second is that the store's layout and credentials stay inside the service. A
job's result is `/jobs/{id}/trajectory`, which is an address that means something
to a person reading it, and nothing about where the service actually keeps its
files is a fact the client learns or could come to depend on.

The cost is real and is accepted knowingly: the API carries the bandwidth of
every download. At one worker producing one trajectory every few minutes that is
not a bottleneck, and the moment it is, the answer is a presigned URL and a CORS
policy, which is a change to one route.

## Alternatives considered

**A `bytea` column.** One system, one backup, transactional with the job's state.
Rejected on the reading pattern above, and on backups. The transactional argument
is weaker than it looks: the worker uploads before it marks the job done, so a
job that says it is finished is one whose result is there, which is the only
ordering property that matters here.

**Large objects.** PostgreSQL's own answer to this, with a streaming interface
and no size limit. It solves the reading pattern and none of the rest, and it is
a facility few people know, which is a poor property for the part of the system
that holds the output.

**The worker's local disc, served by the worker.** No object store at all. It
makes the worker stateful, so it cannot be restarted or replaced without losing
every result it holds, and it puts the download on the machine whose job is to
compute.

## Consequences

The service has an S3 dependency, which is `boto3` and an endpoint. In the local
stack that is MinIO in a container; in a deployment it is whatever the host
provides.

The bucket is created on start-up if it is absent, by both processes, for the
same reason the schema is applied by both: a deployment that needed a separate
step to be usable has a state where the image is running and the step has not
been taken.

A trajectory that is half written never becomes visible. The worker writes to a
temporary directory, uploads when the run has finished, and only then records the
keys, so a job that fails or is killed leaves nothing behind for a client to
fetch. `service/tests/test_worker.py` kills a real worker part way through a run
and asserts exactly that.
