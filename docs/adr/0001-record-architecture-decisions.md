# ADR-0001: Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Orrery is built one phase at a time, and each phase is a separate working
session. Decisions taken in an early phase constrain every later one: the
structure-of-arrays particle layout shapes the force kernels, the build-time
precision switch shapes the SYCL backend, and the choice of one solver behind
several backends shapes what a GPU implementation is allowed to be.

Those decisions are visible in the code only as their outcome. The code shows
what was chosen. It does not show what else was on the table, which constraint
ruled the alternatives out, or whether the constraint still holds. Six months
later that reasoning is gone, and the usual result is either that a decision is
reversed without anyone noticing why it was taken, or that it is preserved out
of caution long after the reason for it has expired.

The project also has a reviewer in mind. A reader who did not write the code
should be able to judge it, and a design choice with no recorded reasoning
cannot be judged, only accepted or doubted.

## Decision

Non-obvious design decisions are recorded as short numbered documents in
`docs/adr/`, each giving context, decision, alternatives and consequences. An
ADR is written when a choice has a credible alternative that a reviewer might
reasonably have expected instead.

An ADR is never edited after it is merged. A decision that changes is captured
in a new ADR that supersedes the old one, and the old one is marked as
superseded.

## Alternatives considered

**Comments in the code.** Comments explain the code they sit next to, which is
the wrong scale for a decision that spans several files or that is about
something the project deliberately does not do. A comment also cannot record a
rejected alternative without becoming a distraction in the middle of a kernel.

**A single design document.** One document describing the current design is
useful, and the implementation plan already serves that purpose. It cannot
serve this one, because a living document is continuously rewritten and so
never shows what was believed at the time a decision was taken. Rejected
options are exactly the material that gets edited out of such a document first.

**Commit messages and pull request discussion.** The reasoning is genuinely
there, but it is indexed by when the change was made rather than by what was
decided. Finding the argument behind a decision means knowing in advance which
change introduced it, which is precisely what a newcomer does not know.

**No record at all.** Viable for a project that no one else will read. It is
not viable for one whose stated goal is engineering that survives inspection.

## Consequences

Every non-obvious decision now costs an extra document, and part of the
judgement in each phase is deciding which decisions clear that bar. Writing an
ADR for everything would bury the ones that matter, so the test is the credible
alternative: if no reviewer would have expected anything else, no ADR is needed.

The immutability rule means the directory accumulates documents that no longer
describe the current design. This is intended. The superseding chain is the
history, and reading it in order is the only way to see how the design moved.

Consequently `docs/adr/` becomes a required stop in review. A pull request that
introduces a non-obvious decision without an ADR is incomplete, and this is
listed in the definition of done.
