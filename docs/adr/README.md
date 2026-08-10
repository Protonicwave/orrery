# Architecture decision records

Short numbered documents recording the decisions behind Orrery's design, each
with its context, the alternatives that were rejected, and what follows from the
choice.

An ADR is written when a decision has a credible alternative that a reviewer
might reasonably have expected instead. It is never edited after it is merged;
a decision that changes is recorded in a new ADR that supersedes the old one.

To add one, copy [`0000-template.md`](0000-template.md), take the next free
number, and link it from the table below and from the pull request that
introduces it.

| Number | Title | Status |
| --- | --- | --- |
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-pin-dependencies-to-exact-commits.md) | Pin dependencies to exact commits fetched at configure time | Accepted |
| [0003](0003-build-settings-through-interface-targets.md) | Carry build settings on interface targets rather than global flags | Accepted |
| [0004](0004-store-particles-as-component-arrays.md) | Store particles as one array per component | Accepted |
| [0005](0005-allocate-particle-arrays-on-cache-lines.md) | Allocate particle arrays on cache-line boundaries | Accepted |
| [0006](0006-select-precision-at-build-time.md) | Select the scalar precision when the project is configured | Accepted |
| [0007](0007-work-in-units-where-g-is-one.md) | Work in units where the gravitational constant is one | Accepted |
| [0008](0008-share-one-softening-definition.md) | Share one softening definition between the solver and the diagnostics | Accepted |
| [0009](0009-generate-the-random-distributions-here.md) | Generate the random distributions in the project rather than with the standard library | Accepted |
| [0010](0010-give-initial-conditions-their-own-layer.md) | Give initial conditions their own layer | Accepted |
