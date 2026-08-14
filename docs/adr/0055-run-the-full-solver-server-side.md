# ADR-0055: Run the full solver server side, over a document rather than a form

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

ADR-0051 put the solver in the browser, and said what that is for: the physics,
demonstrably the same physics, at a size a tab can hold. It is a few thousand
particles on one thread with a scalar kernel, and it is honest about being that.

What it cannot do is the thing this project is about. The runs in
`docs/performance.md` are tens of thousands of particles over tens of thousands
of steps, and a reader who wants to change one of those settings and see what
happens has, until now, had two options: install a C++ toolchain, or believe the
report.

A compute service is the third option. Somebody changes the softening, presses a
button, waits a couple of minutes and watches the result in the same viewer that
plays the published runs.

The design question is what crosses the wire.

## Decision

The service takes the text of an `.orrery` file, and nothing else.

A submission is one field. It is not a form with twenty numbers in it, not a JSON
object mirroring `Configuration`, and not a set of query parameters. The service
parses that text, bound-checks it, writes it to a file, and runs
`orrery run` over it with an argument vector.

This is the same decision ADR-0051 made at the WebAssembly boundary, extended to
the third place a configuration crosses a boundary, and the reasons compound:

**There is one definition of what a run is.** `docs/formats/configuration.md`
specifies it, `src/sim/config_file.cpp` reads it, and the browser, the command
line and the service all read that. A structured submission would have been a
second schema for the same twenty settings, and the two would have drifted the
first time a setting was added.

**A submitted run is a run.** The document the service ran is the document a
person can download, commit, and run natively to get the same trajectory. That
is the property `configuration.hpp` exists to provide, and routing submissions
through a form would have broken it at exactly the point where somebody first
wanted to reproduce something.

**The editor already produces one.** The initial-conditions editor emits a valid
`.orrery` file, because that is the bridge back to the repository. It is
therefore already a submission, and nothing had to be built to make it one.

**It is the smallest surface.** One string in, one trajectory out.

## What the service adds, and what it must not

The service is not allowed to change the physics of what it was sent. It settles
what the submission deliberately does not decide, which is the `[output]`
section: where the trajectory goes, how often a frame is recorded, and how often
the conserved quantities are measured. Those are downloads rather than physics,
they are chosen the way the published gallery's are chosen, and a submission that
states them is refused rather than overridden.

Everything else is a refusal. `orrery_service/validation.py` mirrors
`problems_with` setting for setting and then adds what only a deployment can
know: the ceilings, and the two solvers a worker with no GPU can provide. Nothing
is clamped. A run quietly cut down to what the service felt like giving is a run
whose result answers a question nobody asked, which is the same argument
`docs/formats/configuration.md` makes about a setting the parser might have
skipped.

## The service does not do the physics twice

The worker runs the binary this repository builds, from the revision the image
was built from, and does nothing to its output but move it. It does not
reimplement the run loop through the Python bindings, which would have been a
second driver to keep in step with `apps/orrery.cpp`, and it does not
post-process the trajectory, which is already in a format the client reads.

That is what makes the claim in the first paragraph of this record true rather
than approximately true: a run submitted from a browser is the same run as one
taken from a command line, because it is the same program reading the same
document.

## Alternatives considered

**A structured submission, validated by the API's own schema.** Conventional, and
it would give the client a typed form for free. Rejected: it is a second
definition of a configuration, and the one thing this project has consistently
refused is a second definition of anything that decides a result.

**Run the solver through the Python bindings in the worker process.** The
bindings exist and are tested, so this is not unreasonable. It was rejected
because it needs a driver: the loop, the output writing and the diagnostics
stride are all in `apps/orrery.cpp`, and a second copy of that in Python is
exactly the kind of duplication that produces two programs which agree until they
do not. Running the binary means there is one driver.

**Accept a configuration and a set of overrides, as `--set` does.** Tempting,
since the command line works that way. It adds a second way to say the same
thing and a question about which wins. The client applies its overrides before
submitting, which is what `orrery run --set` is doing anyway.

**Stream the trajectory back over the same connection.** The result is tens of
megabytes and the client already has a reader that fetches ranges of a URL. See
ADR-0057.

## Consequences

The service needs a reader for the configuration format in Python, which is a
third implementation of one specification. That is the cost of validating before
queueing rather than after starting a container, and it is not left as a claim:
`service/tests/test_agreement.py` puts seventy-six configurations through both
that reader and `orrery show` and requires them to agree about which are runs and
which settings are wrong with the rest.

The ceilings are stated as work rather than as seconds, because a second is a
property of the machine a container lands on and this repository has not measured
that machine. What the client shows as an estimate is the service's own median
step time over the jobs it has completed, and before it has completed any, the
laptop's figure with a clause saying whose it is.

Submissions are idempotent by the hash of the settled configuration. The same run
asked for twice, however it was spelled, is queued once and both submissions get
the same result.
