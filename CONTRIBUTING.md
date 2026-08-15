# Contributing to Orrery

This file describes how to work on Orrery. What the project is and what it has
been measured to do are in [the README](README.md) and in the two reports it
links; the reasoning behind the design is in [`docs/adr/`](docs/adr/).

## The shape of a contribution

One logical change, one branch, one pull request. A branch is named after the
change rather than after whoever is making it or when: `feat/quadrupole-moments`,
`fix/checkpoint-alignment`, `docs/trajectory-format`.

`main` is protected and always green. There are no direct pushes to it.

## Language and tone

- UK English throughout: code, comments, documentation, commit messages and pull
  requests.
- Plain English. Short sentences and concrete nouns.
- No em-dashes anywhere.
- No AI attribution of any kind. No generated-by notices, no co-author trailers,
  no tool names in commits, pull request bodies, comments or documentation.

## Code style

`.clang-format` and `.clang-tidy` are authoritative. Formatting is never
discussed in review, and a lint diagnostic is a build error in CI. Beyond what
those two files can express:

- C++20. Clang is the reference compiler. GCC and MSVC must also build cleanly.
- Types `PascalCase`, functions and variables `snake_case`, private members with
  a trailing underscore, constants `kPascalCase`. Macros are avoided entirely.
- Headers live in `include/orrery/<layer>/`, implementation in `src/<layer>/`.
- Include what you use. Include order is own header, C++ standard library, third
  party, project, each block alphabetised. clang-format enforces this.
- RAII everywhere. No owning raw pointers, and no manual `new` or `delete`
  outside the allocator in `core/`.
- `[[nodiscard]]` on every function whose return value is its only effect.
- Pass ranges as `std::span` rather than a pointer and a length, except inside
  kernels where the raw pointer is deliberate and is commented as such.
- Exceptions are permitted at configuration and setup boundaries. No exception
  may propagate out of a kernel or a per-particle loop.

### Layering

Dependencies point downwards only, and no layer may include a header from a
layer above it:

```
apps/                Command-line simulator, viewer
viz/                 Camera, tone mapping, point renderer, window
sim/                 Simulation driver: owns solver, integrator, output, checkpoints
solvers/             Direct O(N^2), Barnes-Hut O(N log N)
integrators/         Velocity Verlet, Yoshida 4th order, RK4
backend/             CPU (work-stealing, AVX2), SYCL (Arc iGPU, unified memory), CUDA
initial_conditions/  Sampled models and analytic configurations
core/                Particle storage, vector maths, diagnostics, memory
```

This is checked at every pull request.

Two directories sit outside that list. `viz/` is a layer, but it sits beside
`sim/` rather than under it: it depends on `core/` alone and `sim/` does not know
it exists, so `apps/` is the one place that names both. `python/` is not a layer
at all. It depends on everything and nothing depends on it, and no C++ target
links it.

### Comments

Comments explain why, not what. A comment that restates the code is noise and
will be rejected in review. The comments that earn their place explain physical
reasoning, numerical trade-offs, and performance decisions that would otherwise
look arbitrary. Every non-obvious constant carries its justification.

## Commits

Conventional commits, with the subject in the imperative and under 72
characters:

```
feat:      new capability
fix:       corrected behaviour
perf:      measured performance change
test:      tests only
docs:      documentation only
build:     build system, dependencies, packaging
ci:        continuous integration
refactor:  behaviour preserved, structure changed
chore:     anything else with no effect on behaviour
```

A commit is a logical unit, not an end-of-session dump. A commit that mixes a
feature with unrelated formatting will be split before it is merged.

## Tests

Four categories, all of which the project needs as it grows:

1. **Unit.** Individual components in isolation, using Catch2.
2. **Property.** Invariants over randomised inputs with fixed seeds. Total
   linear momentum is conserved to round-off for any valid configuration, for
   example.
3. **Validation.** Comparison against known analytic results: Kepler two-body
   orbits, the virial ratio of a sampled Plummer sphere, the convergence order
   of each integrator. This category is the point of the project.
4. **Regression.** Golden outputs with fixed seeds, so that an optimisation
   which silently changes the physics is caught rather than celebrated.

Tests are deterministic. Any randomness is seeded explicitly and the seed is
recorded in the failure message.

## Architecture decision records

Non-obvious decisions are recorded in [`docs/adr/`](docs/adr/) as short numbered
documents giving context, decision and consequences. Write one when a choice has
a credible alternative that a reviewer might reasonably have expected instead.
Copy [`docs/adr/0000-template.md`](docs/adr/0000-template.md) and take the next
free number.

An ADR is never edited after it is merged. A decision that changes is recorded
in a new ADR which supersedes the old one, and the old one is marked as
superseded by it. The record of what was believed at the time is the reason the
directory exists.

## Definition of done

A pull request merges only when all of the following hold:

- Builds clean with Clang, GCC and MSVC, with warnings as errors.
- `clang-format` and `clang-tidy` pass with no diagnostics.
- Address and undefined-behaviour sanitiser builds pass the full test suite.
- All new code is covered by tests in at least one of the four categories.
- Public headers carry documentation comments explaining purpose and rationale.
- Any new non-obvious decision has an ADR.
- Performance-affecting changes report measured before and after figures, taken
  by the benchmark protocol in ADR-0019 that `benchmarks/harness/` implements.
- The README reflects reality. No claim appears there that is not reproducible
  by running a documented command.
- The documentation site builds with no warnings: `doxygen docs/Doxyfile`. A
  cross-reference that has gone dead is a failed build rather than a dead link on
  the published site.
- No AI attribution anywhere in the diff.

## Reporting a problem

Use the issue templates. For anything numerical, the report needs the seed, the
configuration and the compiler and build preset, because without them the
behaviour cannot be reproduced and the report cannot be acted on.
