# ADR-0012: Give the integrators an abstract acceleration field of their own

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

An integrator needs accelerations. The thing that computes them is a solver, and
in the layering of section 3 of the implementation plan `solvers/` sits above
`integrators/`, so a header in the integrators layer may not include one from the
solvers layer. The dependency has to point downwards, which means the abstraction
has to live at or below the integrators.

There is also an ordering problem of the same shape as the one ADR-0008 records.
The integrators arrive in Phase 4 and the direct solver in Phase 5, so whatever
the integrators call has to be written before its first real implementation
exists.

A third constraint comes from RK4. Its stages evaluate the acceleration at
positions that are not the state of any particle: they are intermediate points
that the system never occupies. Whatever the integrator calls therefore has to
accept positions that are not the ones held in the particle store.

## Decision

`integrators/acceleration_field.hpp` declares an abstract class with a single
operation: given positions, masses and an output view, write the acceleration of
each particle. It takes `Vec3Span` and `std::span` rather than a `ParticleData`,
so a caller can point it at stage buffers as easily as at the state. It is not
`const`, so an implementation may keep a counter or a tree.

The solvers of Phase 5 and Phase 8 implement this interface. The integrators
never learn what implements it.

## Alternatives considered

**Take the solver type as a template parameter.** A function template on the
field type removes the indirect call and lets the compiler inline the force
evaluation into the step. The inlining argument is the reason to consider it and
it does not survive contact with the numbers: the call happens once to four times
per step, ahead of N^2 interactions, so the saving is unmeasurable, while the cost
is that every integrator becomes a template, every combination of integrator and
solver is a separate instantiation, and the selection of either from a
command-line flag turns into a switch over the product of the two sets. Section 3
of the plan already settles this: virtual dispatch at boundaries, never inside
loops.

**Pass a `std::function` or a raw callable.** Lighter to write and it keeps the
integrator from naming a base class. It gives up the ability to state the contract
in one place, since a callable has no header where its preconditions are written,
and `std::function` adds an allocation and an indirect call rather than removing
one.

**Declare the interface in `core` instead.** `core` is where shared vocabulary
lives, and the softening precedent is there. Rejected because this is not
vocabulary: it is the seam between two specific layers, and putting it in `core`
would let anything in the project depend on it without saying which side of the
seam it is on. Nothing in `core` needs to know that gravity is computed by
anything at all.

**Have the field take a `ParticleData` and write into its accelerations.** The
simplest signature, and the one that reads best for velocity Verlet, which only
ever evaluates at the real state. It cannot express a Runge-Kutta stage without
either a second particle store held purely to carry stage positions, at a third
more memory than the stage needs, or a temporary swap of the real one, which is a
mutation of the caller's state during a call that is supposed to read it.

**Wait for Phase 5 and let the solver define the interface.** The natural reading
of the rule that a phase implements nothing scheduled for a later one. It inverts
the dependency direction the architecture requires and would leave this phase
unable to test anything, since an integrator with nothing to integrate cannot be
validated. The interface is declared here; every kernel behind it, its threading,
its vectorisation and its interaction counting, remains in the phases that own
them.

## Consequences

The integrators layer depends on `core` alone and can be tested without a solver.
The test suite of this phase supplies its own direct summation field, written for
obviousness rather than for speed, and states plainly that it is a test
instrument.

Phase 5 has one more thing to do: the direct solver implements this interface
rather than inventing its own entry point. That is the intended consequence, and
it is what stops the project from growing two ways to ask for a force.

A GPU backend in Phase 9 is an implementation of the same interface, so the
integrators need no change to run against it. The unified memory of the target
machine makes that a real possibility rather than a hopeful one: the spans a SYCL
kernel reads can be the spans the CPU wrote.

The interface says nothing about softening, which is a property of the field's
implementation rather than of the integrator, and nothing about interaction
counting, which Phase 5 adds to its own class. Both are deliberate: an interface
that named them would make every future implementation carry them.
