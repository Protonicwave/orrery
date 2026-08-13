# ADR-0052: Build the WebAssembly module single threaded, in a Worker

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The library's CPU solvers divide their outer loop between threads through the
executor of ADR-0017, and the work-stealing scheduler of ADR-0016 is what the
performance report was taken with. A configuration file names a scheduler and a
thread count, and every published run names the work-stealing one.

WebAssembly can have threads. They are implemented as Web Workers sharing one
`SharedArrayBuffer`, and a browser will only give a page a `SharedArrayBuffer`
when the page is cross-origin isolated, which requires the server to send
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`.

The site is served by GitHub Pages, which does not send response headers a
repository asks for and has no configuration file that would change that. So the
choice is not between threads and no threads. It is between no threads and a
different host.

## Decision

The module is built without `-pthread` and runs on one thread, inside one
Worker.

The Worker is not a thread of the simulation: it is the thread the simulation is
on, and the page is the one that draws. That division is the same one ADR-0047
made for decoding, and for the same reason. A step at the module's particle
limit takes tens of milliseconds and cannot be interrupted once it is on the
stack, so a step taken on the page's thread would be a frame during which
nothing draws, no control answers and no scroll moves.

A configuration naming a scheduler this build does not have is run anyway, on one
thread, and told so. That is the same answer `sim/assembly.cpp` already gives to
a configuration asking for a GPU that is not there: the request is a request,
the fallback computes the same thing more slowly, and what actually happened is
reported rather than assumed. The report reaches the screen, not a log.

The loop inside the Worker is a chain of scheduled tasks, one recorded frame
each, rather than a loop that runs to the end of the run. A Worker reads its
message queue between tasks and never during one, so a run that stepped to
completion in a single task could not be stopped, and the stop button would be a
control that does nothing until the work it was meant to cancel had finished.

## The particle limit follows from the thread count

One thread with the scalar kernel is roughly two orders of magnitude short of
what the same machine does natively, and the direct solver is quadratic. Measured
in Chromium on the development machine, at four thousand and ninety-six particles
a direct step takes 45 ms and a Barnes-Hut step 28 ms. The next power of two
would take four times the first.

Four thousand and ninety-six is therefore the module's limit, and it is enforced
in the module rather than in the client: `orrery_particle_limit` reports it, the
client asks before it writes a configuration, and a configuration that asks for
more is refused with a sentence saying by how much. A limit stated in one place
and enforced in another is a limit that will one day differ.

## Alternatives considered

**Host the site somewhere that sends the headers.** Cloudflare Pages, Netlify and
a hundred others will. It would buy four or eight threads and cost the property
that the whole system is published by the same repository that builds it, which
is what ADR-0045 chose the monorepo for. It would also make the browser build
faster in a way that invites exactly the comparison the browser build must not
invite: the point of it is the physics, and the figures are the native ones.

**Ship two modules, one threaded and one not, and pick at run time.** Doable, and
it doubles the download for the case where the headers are present, which here is
never.

**Use several Workers, each with its own module, and divide the particles.**
Threads without shared memory. Every step would then need each Worker to send
every other Worker its positions, which for a direct solver is the whole state
per step per pair of Workers. That is more traffic than the arithmetic it would
parallelise.

**Run the module on the page's thread and keep the steps small.** The step size
is set by the particle count, not by preference, and cutting the particle count
until a step fits in a frame budget would put the limit near five hundred
particles. It would also mean the render loop and the solver competing for one
thread, so the frame rate would fall as the run got interesting.

## Consequences

`ORRERY_SINGLE_PRECISION` is off in this build, so the module is the
double-precision one and the boundary reports `orrery_scalar_size` as eight. It
is the accuracy-oriented configuration because agreement with the native build is
the claim; positions are narrowed to `float` on the way out, because what reads
them is a renderer.

The executor layer is compiled into the module and never used beyond the serial
executor, which is a small amount of the download spent on keeping `src/`
untouched. Excluding the other three would have meant a preprocessor condition in
`sim/assembly.cpp`, which is shared code, to remove the only three lines that
name them.

The module holds one run at a time. Nothing in the interface prevents a second,
but the Worker has one, because a tab that could integrate two runs at once on
one thread would run both at half speed and report neither honestly.
