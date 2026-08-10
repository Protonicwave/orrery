# ADR-0008: Share one softening definition between the solver and the diagnostics

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Gravitational simulations soften the force between particles, replacing the
point-mass potential with one that stays finite as the separation goes to zero.
This project uses Plummer softening, which replaces a point mass with the
potential of a Plummer sphere of scale radius `eps`:

    Phi(r) = -G m / sqrt(r^2 + eps^2)

The force kernel of Phase 5 will differentiate this and the potential energy
diagnostic of this phase integrates it. They are two expressions of one choice,
and they have to agree.

The implementation plan states the requirement directly: the potential energy
calculation must use the same softening as the force kernel, otherwise the
conservation tests measure an artefact of the diagnostic rather than the
physics. The failure this guards against is not hypothetical and it is not
loud. A diagnostic that omitted the softening, or that used a slightly different
functional form for it, would report an energy that drifts under a perfectly
symplectic integrator, and the natural conclusion on seeing that plot would be
that the integrator is wrong.

The awkwardness is one of ordering. The diagnostics arrive in Phase 3 and the
force kernel in Phase 5, so the shared definition has to be written before one
of its two users exists.

## Decision

`core/softening.hpp` holds the `Softening` type and both functional forms: the
factor `1 / sqrt(r^2 + eps^2)` that the potential energy uses, and the factor
`1 / (r^2 + eps^2)^(3/2)` that the acceleration uses. The second is written as
the cube of the first, so the two are built from bit-identical values of the
softened distance rather than from two independent square roots.

A property test in this phase checks the acceleration form against a finite
difference of the potential form, so the pairing is asserted rather than
asserted-in-a-comment. Phase 5 consumes this header rather than deciding the
question again.

## Alternatives considered

**Define the softening in the solver and have the diagnostics include it.** The
natural placement, since the kernel is the hot user. It inverts the dependency
direction the implementation plan requires, because `core` may not include from
`solvers`, and it would mean the diagnostics of Phase 3 could not be written
until Phase 5 existed.

**Let each side write its own expression.** Two lines of arithmetic, each
obvious, in the places that use them. This is the option the plan warns about
explicitly, and it fails silently rather than loudly: the two agree for a
softening of zero, which is what most early tests use, so the disagreement would
first appear in exactly the long softened run whose result matters most.

**Pass the softening as a bare `Real` and share only the number.** Cheaper than
a type and it addresses half the problem, since the two sides would at least
agree on the parameter. It leaves the functional form unshared, which is the
half that matters: a kernel dividing by `r^2 + eps^2` once too few or once too
many times is still consistent about `eps`.

**Defer the second form to Phase 5, as the phase boundary implies.** The
tidiest reading of the plan's rule that a phase implements nothing scheduled for
a later one. It cannot satisfy the plan's other requirement, which is that this
phase's potential energy use the same softening as a kernel that does not yet
exist. Writing both forms together, with a test tying them, is the only way to
make that a structural guarantee rather than a note for a future reader. The
force kernel itself, its threading, its vectorisation and its interaction
counting all remain in Phase 5.

## Consequences

The softening length is a property of a run rather than of a kernel, which is
what it physically is: it says how large the mass distribution each particle
represents actually is.

`core` now contains a function that nothing in `core` calls, the acceleration
factor, until Phase 5 arrives. That is the visible cost of the decision, and it
is why the header states the relationship between the two forms rather than
presenting them as two utilities.

The finite-difference test is a real constraint on future changes. Any
adjustment to either form that breaks the derivative relationship fails a test
in this phase rather than showing up as an unexplained drift in a later one.

Softening of zero remains available and is the default, so the analytic
comparisons against point-mass results, the Kepler orbit above all, are made
against the physics they are actually claims about.
