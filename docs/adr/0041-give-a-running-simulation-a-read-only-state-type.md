# ADR-0041: Give a running simulation's state a read-only type of its own

- **Status:** Accepted
- **Date:** 2026-08-12

## Context

`Simulation::particles` returns `const core::ParticleData&`, and the constness
is load-bearing rather than habitual. ADR-0013 makes it an interface invariant
that the acceleration array holds the acceleration at the current positions on
entry to every step, and every integrator in the project relies on it. A caller
that moved a particle between steps would leave the accelerations belonging to
where the particles used to be, and the next step would integrate one step of
wrong physics and then quietly recover. Nothing would raise and nothing in the
output would say which step was wrong.

In C++ that cannot happen, because there is no way to write through the
reference. A Python object carries no constness: if `Simulation.particles`
returned the same bound type as a store the caller built itself, the same ten
writable arrays would appear on both, and the invariant would be protected in
one language and not in the other.

## Decision

There are two bound types. `ParticleData` owns a store and hands out writable
views. `ParticleView` refers to a store owned by something else and hands out
views with NumPy's `writeable` flag cleared, so an attempt to write raises
rather than corrupting a run.

`Simulation.particles` returns a `ParticleView`. The supported way to put a
state into a simulation is `Simulation.restore`, which re-establishes the
invariant as part of accepting it.

## Alternatives considered

**One type, writable, with the hazard documented.** Fewer moving parts, and a
scientific package whose users expect to poke at state. It was rejected because
the documentation would have to say "writing here silently produces one step of
wrong physics", and a footgun with a warning label beside it is still a
footgun. The C++ interface already decided this question; the binding should
not decide it differently.

**One type, writable, with the simulation refreshing its accelerations on every
step.** This removes the hazard by removing the invariant, at the cost of an
extra force evaluation per step for velocity Verlet, which is a doubling of the
cost of the whole simulation. ADR-0013 weighed exactly this.

**One type, and return a copy from `Simulation.particles`.** Safe, simple, and
it copies the entire state on every access. A notebook plotting every hundredth
frame of a million-particle run would copy eighty megabytes each time, which is
the expense this phase exists to remove.

**A `writable` flag on the one type, set when it is constructed.** One class
instead of two, at the cost of a run-time flag that every one of the ten
property getters has to consult and that says nothing in its name. Two types
put the distinction in the type system, which is where this project puts
distinctions that matter.

## Consequences

There are two classes with ten parallel properties, which is duplication in the
binding source. It is deliberate and it is bounded: the ten names are the ten
component arrays, and an eleventh would be a change to `ParticleData` in
`core/`, which already has one place that lists them.

`ParticleView.copy()` produces a writable `ParticleData`, so the workflow of
taking a state out, changing it and putting it back is available and is three
explicit steps rather than one implicit one.

A `ParticleView` holds a reference to the Python object that owns the storage,
so the simulation cannot be collected while a view or an array taken from it is
alive. It does not survive a reallocation: `Simulation.restore` may change the
particle count, and a view taken before it points at freed memory afterwards.
That is the same rule a `std::span` follows, it cannot be detected from inside
NumPy, and it is documented on the operations that cause it.
