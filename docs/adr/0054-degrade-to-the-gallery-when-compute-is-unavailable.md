# ADR-0054: Degrade to the gallery when compute is unavailable

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

The instrument can now ask a compute service for a run. That service is one
container on modest hardware with one worker, reached over the public internet
from a page served by GitHub Pages, and it is the least reliable thing in this
project by a wide margin. It will be down. It will be busy. It will sometimes be
deployed nowhere at all, because the site is published on every push to `main`
and the service is not.

So the question is not whether the client will meet a service it cannot use. It
is what the client does on that day, and there are two answers.

The first is an error state: the page says the service is unavailable, and the
solver tier, which is a third of the console, becomes a red box. Everything else
still works, but the page now reads as broken, because a visible failure is the
loudest thing on a screen and a reader does not weigh it against the parts that
are fine.

The second is to notice that the instrument was complete before the service
existed. It plays three published runs, drawn from files this repository
produced, with every figure on screen read out of one of them. None of that
needs a network beyond the one that served the page.

## Decision

A compute service that cannot be reached is a state the interface describes
rather than fails in. Specifically:

The client asks `/capabilities` before it offers to submit anything, and treats
four answers as the same thing: no service was configured into this build, the
service did not answer, no worker has beaten recently, and the queue is full.
Each produces a different sentence and the same behaviour. The solver tier's
controls are drawn back with the reason attached, exactly as the controls that a
trajectory cannot support already are, and the sentence ends by saying that the
published runs are unaffected.

Nothing else on the page changes. The plate, the transport, the data rail, the
gallery and the reading half do not consult the service and do not know whether
it exists.

The service refuses rather than queues when it cannot serve. A submission
arriving while no worker is alive is answered 503 with a sentence, not accepted
into a queue nothing is draining. Somebody told "no" reads the gallery; somebody
given a job that never moves waits, and then decides the whole thing is broken.

## Why this is engineering rather than presentation

It is tempting to file graceful degradation under polish. It is not, and the
argument is the same one `problems_with` makes about configuration errors: the
system knows something the person does not, and the whole question is whether it
says so usefully.

There is also a self-interested reason. This client's most valuable property is
that every number on it comes from a file this repository produced. That
property is completely unaffected by the service being down, and an interface
that presented itself as broken in that state would be understating what it is.

## Alternatives considered

**Show an error and disable the console.** Honest and unhelpful. It states the
one thing that is wrong and says nothing about the many things that are not.

**Hide the solver tier when there is no service.** Worse. The tier's notes are
the most useful writing in the interface, because between them they say exactly
what a trajectory is and what computing a new one would need. Removing them on
the day the service is down removes the explanation at the moment it is most
wanted, and it makes the page silently different for different readers.

**Queue the submission and run it when a worker returns.** This is a real design
and it is the wrong one at this scale. It needs the client to remember a job
across a page load, and it means the answer to "how long will this take" is
"until somebody restarts the worker". With one worker and no accounts, a refusal
now is more use than a promise later.

**Fall back to the WebAssembly solver.** The browser build is already there, one
control below, and it would quietly substitute a run of a few thousand particles
on one thread for the run that was asked for. Two solvers behind one button,
with the picture depending on which happened to be reachable, is the one thing
the plate's catalogue exists to prevent. The two controls stay separate and each
says what it is.

## Consequences

The published site is built without a compute service and is therefore always in
the degraded state. That is not a workaround: it means the state is on screen
every day rather than only during an incident, and
`web/e2e/console.spec.ts` asserts it, so it cannot rot unnoticed.

The client has no retry loop and no queue of its own. A submission is refused or
accepted when it is made.

The capabilities are kept when the service stops answering, so the console can go
on stating the ceilings it last learned while saying it cannot be reached. What
it must not do is offer to submit against them, and it does not.
