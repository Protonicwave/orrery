# ADR-0045: Keep the browser client in this repository

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The project has a simulator, a set of bindings and a native viewer, and it is
acquiring a browser client that reads the same runs: the configuration a run was
given, the trajectory it produced, the diagnostics beside it, and eventually the
same solver compiled to WebAssembly. That client is TypeScript and CSS, and it is
built by a toolchain that has nothing to do with CMake.

The usual arrangement is a repository of its own. A front end has its own
dependencies, its own release rhythm and its own reviewers, and putting it beside
a C++ library means one checkout that two different toolchains have to be
installed to build.

The question is where the seam belongs. The client is not a consumer of this
project in the way an unrelated application would be: every number it displays is
a figure this repository produced, every setting it shows is parsed out of a file
in `examples/`, and the version it states is the one in `CMakeLists.txt`. A seam
between them is a seam through the middle of one claim.

## Decision

The client lives in `web/`, beside `src/`, `python/` and `docs/`, and is part of
the same repository and the same pull requests.

It reads the repository directly rather than holding copies. The data rail is
rendered from `examples/collision.orrery`, parsed at build time by a reader in
the client written against `docs/formats/configuration.md`; the version in the
masthead is read from the `project()` call in `CMakeLists.txt`; and the measured
figures it shows carry the conditions the reports state them under. A change to
the configuration changes the interface, and a change that would make the
interface state something untrue fails a test in `web/tests/`.

It is published from the same GitHub Pages deployment as the documentation,
which is the only one a repository has: the documentation site stays at the root
and the client is served from `/instrument/`, assembled into one artefact by the
site workflow. The client is built and its size budget asserted on every pull
request, as the documentation is built on every pull request.

## Alternatives considered

**A separate repository.** The conventional answer, and it would give the client
its own issue tracker and its own release history. It would also put a network
boundary between the interface and the figures it states: the configuration, the
version and the performance results would have to be copied across, or published
as an artefact and consumed, and the day one of them changed the other would be
wrong until somebody noticed. Every reason this repository holds its own
documentation rather than a link to it applies here.

**A workspace or a submodule.** A submodule keeps the two histories separate and
still checks out together. It also means a pointer commit for every change, a
checkout that is wrong by default, and a reviewer reading a pull request that
says only that the pointer moved.

**Publishing the client in place of the documentation.** The client is the more
interesting half to arrive at, and the root is where a visitor arrives. It is not
yet the more useful half, and moving a published site is a decision that should
be taken once, when there is something at the new address worth being sent to.
Until then the documentation keeps the address it has and the client sits beside
it.

**Keeping the client's figures in a data file of its own.** Simpler to build, and
it is what most interfaces do. It is also how an interface comes to state a
particle count the configuration no longer asks for. The rule against duplicated
truth is worth more here than the build-time cost of parsing one file.

## Consequences

A checkout builds either half. Neither toolchain is needed to work on the other:
`cmake --preset release` does not know `web/` exists, and `npm run build` never
compiles anything. Continuous integration runs both, and a pull request that
touches only C++ still pays for a Node install on one job.

The client can reach files above its own directory, which needs Vite to be told
so. That allowance is for the repository's own configuration files and is stated
in `web/vite.config.ts`, so what crosses the boundary is a short list rather than
a habit.

One deployment carries both halves, so a change to either publishes both. That is
the behaviour a single Pages deployment has, and the alternative, two workflows
deploying in turn, is not two sites but one site that alternates.

The decision holds for what the client grows into. A renderer, a WebAssembly
build of the solver and a service that runs it are all closer to the C++ than to
the interface, and each of them would have to cross the boundary this decision
declines to draw.
