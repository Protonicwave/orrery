# ADR-0058: Bound the service with the jobs it already stores

- **Status:** Accepted
- **Date:** 2026-08-15

## Context

The service takes a configuration from anybody, with no account and no payment,
and spends minutes of a machine on it. The ceilings written in Phase 8 bound one
run: at most twenty thousand particles, at most twenty thousand steps, and at
most as much work as the demonstration in `README.md`. Every one of them is a
statement about a single submission.

Nothing bounded the sequence. A thousand submissions each inside every ceiling
is a thousand legal runs, and the bill for them is the same whether they came
from a thousand readers or from one script. Making the service public without
answering that is the difference between a demonstration and an open account.

Four questions have to be answered, and each of them is about a set of jobs
rather than about one: how many runs one submitter may queue in an hour, how
much computing the whole service will take on in a day, how many runs may be in
progress at once, and how large a request body will be read before any of it is
parsed.

## Decision

The first three are counted over the `job` table, which is where the jobs
already are, with three columns added to it: who submitted (a salted hash of the
address), what the run costs in the work units `limits.py` already scores, and
when it was submitted, which was there. The rate limit is a count of a
submitter's rows inside a window, the budget is a sum of the work column inside
a longer one, and the concurrency cap is a condition on the claim, which is the
only statement in the system where a run begins.

The fourth is a middleware reading the declared length of the body.

Every ceiling refuses rather than clamps, and says which ceiling it is and what
passed it, which is the rule `docs/formats/configuration.md` sets for the
configuration reader and `limits.py` continues.

## Alternatives considered

**Redis, or a rate-limiting proxy in front.** The usual answer, and it is a
second service to run, to secure and to reason about, for a counter that is one
indexed query. It would also put the limit somewhere the service cannot explain:
a proxy answering 429 does not know how many runs the ceiling allows or what the
gallery is, and the refusal a reader gets would stop being a sentence about this
service. ADR-0056 declined a broker for the queue on the same grounds and this
is the same argument one layer out.

**A token bucket in each API process.** No database, fast, and wrong the moment
there are two API containers, since each would allow the whole rate. It also
forgets everything when a process restarts, which makes the limit a function of
how often the service is deployed.

**Storing the address rather than a hash of it.** Simpler, and it would mean
this service kept a list of who had used it. There are no accounts here and
nothing else about a submitter is recorded, so keeping addresses would be the
one place the service held something a person could be identified from. What a
rate limit needs is to tell two submitters apart, which a salted hash does.

**A ceiling in seconds rather than in work units.** A budget of so many minutes
of computing a day is the figure an operator wants. It is also a property of the
machine the container landed on, which this repository has not measured, and
`limits.py` refuses to state numbers it cannot reproduce. Work units are a
property of the run, they are what the per-submission ceiling already uses, and
the service publishes its own measured step time separately so that a client can
turn the two into an estimate.

**Charging duplicate submissions against the rate.** The same configuration
submitted twice returns the first job and runs nothing, so counting it would be
a ceiling on asking rather than on cost.

## Consequences

The rate limit is exact only for as long as the salt lives. A deployment that
changes it starts everybody's hour again, which is the right failure: the
alternative is a salt that never changes and a hash that is therefore a stable
identifier for a visitor.

The concurrency cap holds to within the number of workers claiming in the same
instant, because two that read a count below the cap will both take a job. That
is a deployment briefly running one more simulation than it meant to. The sharp
ceiling on a day is the budget, which is checked before anything is written.

The budget is charged when a run is accepted rather than when it finishes, so a
queue of accepted runs has already spent it. That is deliberate: the point is to
refuse the next submission before a machine is committed to it, and a budget
measured on completion would let thirty runs be queued against a budget that has
nothing left.

A body whose length is not declared is refused. Everything that legitimately
submits here sends a string of known length, and the alternative is a ceiling
that cannot be enforced by anybody sending a chunked upload.
