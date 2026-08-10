# ADR-0013: Carry the acceleration between steps as an interface invariant

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Velocity Verlet in kick-drift-kick form uses the acceleration twice: once at the
start of the step and once at the end. The acceleration at the start of a step is
the acceleration at the end of the previous one, computed at the same positions
from the same masses, so it is the same number. Recomputing it would make the
method cost two force evaluations a step instead of one, which on this problem
means doubling the cost of the whole simulation, since the force evaluation is
essentially all of it.

The same holds for the other two methods. Yoshida's composition is three velocity
Verlet substeps and inherits the saving three times over. RK4's first stage needs
the derivative at the starting state, which is the same value again, and its
fourth evaluation is the one that produces the acceleration at the new positions,
so with the carry it costs the classical four rather than five.

The saving is only available if the acceleration survives from one call to the
next, and something has to be responsible for that. `ParticleData` already has an
acceleration array, so the storage exists; what is missing is a statement about
when its contents are meaningful.

## Decision

The `Integrator` interface states one invariant:

    `accelerations()` holds the acceleration at `positions()`.

`Integrator::prepare` establishes it. Every `step` requires it on entry and
restores it on exit. A caller that moves particles behind the integrator's back,
or that changes the masses or the field, calls `prepare` again.

The rule is documented at the interface rather than inside each method, and a
unit test asserts it directly: after two steps, the accelerations in the store are
compared bit for bit against a fresh evaluation of the field at the final
positions.

## Alternatives considered

**Have each step evaluate the acceleration it needs.** No invariant, no
precondition, no way to use an integrator wrongly. It costs velocity Verlet a
factor of two and Yoshida a factor of two, on the single operation that dominates
the run. On the target machine and at the particle counts this project is aimed
at, that is the difference between a simulation that is worth running and one
that is not.

**Keep a private copy of the acceleration inside the integrator.** The state
becomes the integrator's own, so no caller can invalidate it, and `prepare`
disappears. It doubles the acceleration storage, which is 24 bytes per particle
in double precision and is the third-largest array in the project, and it does so
to hold a second copy of numbers the particle store already has room for. It also
makes swapping integrators mid-run silently wrong rather than a matter of calling
`prepare`.

**Cache inside the integrator with a validity flag on the particle store.** A
version counter that `ParticleData` bumps on every mutation, checked by the
integrator. It answers the question automatically and puts a check in a hot path
to detect a mistake that the type system cannot make in the first place, since
the only way to invalidate the cache is to write to the store deliberately.
`ParticleData` would also acquire a concept, staleness, that has nothing to do
with storing particles.

**Let the acceleration array be an output only, and evaluate at the top of each
step.** This is the first alternative in a different disguise, and it is what the
plainest reading of the physics suggests. The reason it is not free is that the
last thing a kick-drift-kick step does is evaluate the field at the final
positions, so the value is already sitting there.

## Consequences

An integrator has a precondition, which is a thing a reviewer has to notice, so it
is stated at the top of the interface header and repeated in the doc comment of
every method that depends on it.

The invariant is exactly the kind of thing that decays silently. An integrator
that left a stale acceleration would still produce plausible-looking output, with
an error of order the timestep that no single-step test would catch. It is
therefore asserted by a test of its own rather than trusted, and the comparison is
for exact equality, since the same field at the same positions produces the same
bits and any difference at all is staleness rather than rounding.

Phase 11's checkpoint and restart has to save the accelerations or call `prepare`
after loading. Calling `prepare` is the better answer and costs one evaluation at
the start of a resumed run, which is why the interface has the method at all.

The invariant is what makes the reported `force_evaluations_per_step` figures
true: one, three and four. Those are the numbers every comparison between methods
in this project is normalised by, and a test asserts each of them against a
counting field.
