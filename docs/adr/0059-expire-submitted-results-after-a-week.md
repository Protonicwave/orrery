# ADR-0059: Expire submitted results after a week

- **Status:** Accepted
- **Date:** 2026-08-15

## Context

A submitted run leaves a trajectory of tens of megabytes and a diagnostics file
beside it, and ADR-0057 puts both in object storage. Nothing removed them. A
bucket that only grows is a bill that grows with the number of visitors, and
most of what is in it is a run somebody watched once on an afternoon and never
asked for again: there are no accounts, so nobody can come back to a result they
submitted from another machine, and the client keeps a job for as long as the
tab is open.

The gallery is not in this position. Those runs are the repository's own output,
they are built into the site as static assets, and they are the answer to what
somebody should look at when the service is not available at all.

## Decision

Results of submitted runs are removed seven days after the run finished. A sweep
in the API deletes the objects and then marks the row, hourly, a bounded batch
at a time. A lifecycle rule on the bucket's `jobs/` prefix is set at start-up as
a backstop.

A job whose result has expired keeps its row and stops offering somewhere to
fetch from, and the two download routes say that the result was removed and how
long results are kept, rather than that the run produced nothing. Submitting the
same configuration again queues it again instead of returning the finished job
it matches.

## Alternatives considered

**The bucket's lifecycle rule alone.** One line of configuration and no code,
and it leaves the database saying a result is there for up to a day after it has
gone, because the rule's granularity is a day and nothing tells the service when
it fires. The client would then be handed a URL that answers 404. Keeping the
service the authority and the rule the backstop costs a sweep and means the two
halves agree.

**A sweep alone.** It would miss what it cannot see: an object uploaded by a
worker that died before it recorded the upload is an object no row names, and
nothing would ever delete it. That case is exactly what a prefix rule catches.

**Keeping everything.** Storage is cheap until it is not, and the growth is
unbounded in the number of visitors rather than in anything about the project.

**Keeping the last N runs.** A cap on count rather than on age. It keeps a run
from a year ago and expires one from this morning after a busy afternoon, which
is the wrong way round for a service whose results are watched as they are
produced.

**Deleting the job rows as well.** Then a link to a job would 404 rather than
say what happened, the record of what the service has run would disappear, and
the median step time it prices new runs with would be computed over a shrinking
window. The rows are small and the trajectories are not, so the large half goes
and the record stays.

## Consequences

Idempotency by content hash now has an exception: a configuration whose earlier
run has expired is run again. That is the honest behaviour, since a finished job
with nothing to fetch is not an answer, and it means the property "the same run
is never run twice" holds only inside the retention window.

A deployment that wants results kept longer changes one number, and the bucket
rule follows it at the granularity of a day.

The sweep runs in the API rather than in the worker, for the reason the reaper
does: the API is the process that is always up, and a worker in the middle of a
run should be doing nothing else.
