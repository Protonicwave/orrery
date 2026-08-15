# The compute service

The instrument plays runs this repository produced. The editor designs one and
hands back a configuration file. The service is what closes the loop: it takes
that file, runs it on hardware that is not the reader's, and gives back a
trajectory the instrument plays through the same viewer it plays the published
gallery through.

It exists because the browser build cannot do what this project is about. The
WebAssembly solver is a few thousand particles on one thread with a scalar
kernel, and it is honest about being that (ADR-0051). A reader who wants to
change the softening on a real run and see what happens has otherwise to install
a C++ toolchain or believe the report.

## What a run is, from one end to the other

1. **A configuration is submitted.** One field: the text of an `.orrery` file,
   exactly as `docs/formats/configuration.md` specifies it and exactly as the
   editor emits it. Not a form, not a structure of twenty numbers. ADR-0055 sets
   out why the whole boundary is one document.
2. **It is read and bound-checked.** The service applies the same rules
   `src/sim/configuration.cpp` applies, and then the ones only a deployment can
   have: the ceilings below, and the two solvers a worker with no GPU can
   provide. Everything wrong with a submission comes back at once, each objection
   naming the setting in the spelling the file uses.
3. **It is queued.** In PostgreSQL, claimed by a worker with
   `SELECT ... FOR UPDATE SKIP LOCKED` (ADR-0056). A submission that matches one
   already queued or already run is not run twice: jobs are identified by the
   hash of the configuration they run, so the same document submitted twice
   returns the first result.
4. **A worker runs it.** The native `orrery` binary, built from the revision the
   worker's image was built from, over the configuration written to a file. The
   worker reads what the run prints and the diagnostics file it writes, and
   reports the step, the model time, the step time and the energy drift as they
   happen.
5. **The result is stored and then recorded.** The trajectory and the
   diagnostics go to object storage, and only then is the job marked done
   (ADR-0057). A job that says it has finished is one whose result is there.
6. **The client plays it.** A finished job is a trajectory at a URL, so the
   reader that plays a published run plays this one, ranged fetches and all.

## What it will not take

Refused, not clamped. A run quietly cut down to what the service felt like
giving would produce a picture of a scenario nobody asked about, and the refusal
names the setting so it can be read beside the file that caused it.

| Ceiling | Value | Why it is there |
| --- | --- | --- |
| Particles | 20,000 | The largest count this repository publishes a measured step time for, in the demonstration in `README.md`. Above it, nothing here has measured the cost |
| Steps | 20,000 | Independent of the count, because a small run integrated for a long time is cheap and reasonable to ask for |
| Work | 20,000 particles over 6,000 steps | The demonstration itself, which is the largest run the repository states a wall clock for. Fewer particles buys more steps |
| Queue | 32 runs | A queue longer than this is one whose last entry will not be reached while anybody is still watching |

Work is scored on the curve the named solver follows: the count times its
logarithm for the tree, and the square of the count for the direct solver, which
computes every pair. One score for both would have to be wrong about one of
them.

Those four bound one run. Four more bound the sequence of them, because a
thousand submissions each inside every ceiling is a thousand legal runs and the
bill is the same whether they came from a thousand readers or from one script.
ADR-0058 is the argument for measuring all of them over the jobs the service
already stores rather than in a second system.

| Ceiling | Value | Why it is there |
| --- | --- | --- |
| Runs from one address | 6 an hour | A run the size of the demonstration took 122 seconds on the machine the performance report names, so six is more than an hour of a worker and more than one person watches. It is loose enough for the loop the editor is for |
| Work in a day | 24 full-size runs | The only ceiling on what a day of strangers costs. On the one wall clock this repository has measured, about fifty minutes of computing |
| Runs at once | 2 | The deployment runs one worker and one run at a time. The second is so that a rollout with an old worker and a new one does not stop taking runs |
| Request body | 64 kB | Four times the configuration ceiling, so that the escaping around a legal configuration cannot fail here, checked before any of the body is read |

The address a submission came from is not kept. What the rate limit needs is to
tell two submitters apart, so the job carries a salted hash and the service
holds nothing a person could later be identified from.

The two SYCL solvers are refused. The parser accepts them in every build, because
a configuration file is a document and should mean the same thing whatever reads
it; whether a particular machine can provide what it asks for is a different
question, and the worker's answer is no. The GPU figures in
[the performance report](performance.md) were measured on the laptop that report
names, not here.

The `[output]` section is refused as well. Where a run writes is the service's
business: the result is fetched from the job rather than written to a path the
submitter chose. The service settles the strides the way the published gallery's
are settled, at about four hundred frames and a hundred diagnostics samples,
because both are downloads rather than physics.

## What it costs, and where that figure comes from

The console's button carries an estimate. It is one measured step time scaled
along the tree solver's curve, and which measured step time matters:

- Once the service has completed a run, its own median. That is the machine that
  would take the next one.
- Before then, 20.4 ms a step at twenty thousand particles, from the
  demonstration in `README.md`, with a clause saying it belongs to this
  project's laptop rather than to the service.

There is deliberately no ceiling expressed in seconds. A second is a property of
the machine a container lands on, and this repository has not measured that
machine, so a figure in seconds would be one it could not reproduce.

## How long a result lasts

Seven days from the run finishing, and then the trajectory and the diagnostics
are removed from storage. A sweep in the API deletes the objects and marks the
job, and a lifecycle rule on the bucket catches anything the sweep cannot see,
such as an upload from a worker that died before it recorded one. ADR-0059 sets
out why it is done in that order rather than by the rule alone.

The job itself stays. Asking about an expired run says what it was, that it
finished, and that its result was removed, which is a different thing to be told
than that the run produced nothing. Submitting the same configuration again runs
it again: jobs are otherwise identified by the hash of what they run, and that
answer is only useful while the first result is still there.

The published gallery is not affected by any of this. Those runs are static
assets built into the site, not jobs in the service's store.

## What it costs to run

The worker gets two cores and two gigabytes, and it is on a network with no
route off the machine: it reaches the database and the object store and nothing
else, since it is the process that runs a program over a document a stranger
sent. The API gets one core and half a gigabyte. Both containers run as an
unprivileged user, with a read-only root filesystem and no capabilities.

That is also the honest figure behind everything above. The GPU numbers in
[the performance report](performance.md) were measured on a laptop with a
discrete card; a run submitted here is two shared cores, and the service
publishes its own median step time so that nothing has to be inferred from
figures that belong to different hardware.

`deploy/README.md` is the operator's document: what a host needs, what each
container is allowed to do, how the metadata database is backed up and how a
restore is taken.

## When it is not there

The service is one container with one worker, reached over the public internet
from a page served by GitHub Pages. It will sometimes be down, busy, or deployed
nowhere at all.

The client treats four answers as the same thing: no service was configured into
this build, the service did not answer, no worker has beaten recently, and the
queue is full. Each produces a different sentence and the same behaviour. The
solver tier's controls are drawn back with the reason attached, and the sentence
says that the published runs are unaffected, which is true: every figure in the
instrument comes from a file in this repository, and none of it needs a network
beyond the one that served the page. ADR-0054 is the argument, and the published
site is built without a service, so that state is the one most readers see.

## Running it

`service/README.md` has the operator's half: the routes, the environment
variables, the compose stack and how to take the tests. In summary:

```
docker compose -f deploy/compose.yaml up --build
```

brings up PostgreSQL, an object store, the API and one worker.

## The contract has one source

`service/orrery_service/contract.py` is the only statement of what crosses
between the client and the service. `web/src/service/contract.ts` is generated
from it, and continuous integration regenerates it and fails if the result
differs from what is committed. A field added on one side and not the other
fails the build rather than reaching a client that does not know about it.

## The decisions

- [ADR-0054](adr/0054-degrade-to-the-gallery-when-compute-is-unavailable.md),
  degrade to the gallery when compute is unavailable.
- [ADR-0055](adr/0055-run-the-full-solver-server-side.md), run the full solver
  server side, over a document rather than a form.
- [ADR-0056](adr/0056-keep-the-job-queue-in-the-database.md), keep the job queue
  in the database that already holds the jobs.
- [ADR-0057](adr/0057-store-trajectories-outside-the-database.md), store
  trajectories outside the database, and serve them through the API.
- [ADR-0058](adr/0058-bound-the-service-with-the-jobs-it-already-stores.md),
  bound the service with the jobs it already stores.
- [ADR-0059](adr/0059-expire-submitted-results-after-a-week.md), expire
  submitted results after a week.
