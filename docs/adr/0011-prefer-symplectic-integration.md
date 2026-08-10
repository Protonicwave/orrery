# ADR-0011: Prefer a symplectic second-order integrator to a non-symplectic fourth-order one

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The obvious way to choose an integrator is by order of accuracy. RK4 is fourth
order and velocity Verlet is second, so RK4 is the more accurate method per step
by two powers of the timestep, and for most numerical work that settles the
question.

It does not settle it here, because of what an N-body simulation is used for. A
run is not an evaluation of one trajectory over a short interval. It is millions
of steps over hundreds or thousands of dynamical times, and the question asked of
the result is whether the system's structure is still the one the physics
implies: whether a cluster is still bound, whether its energy is the energy it
started with, whether an orbit has decayed. Those are statements about the long
run, and the error behaviour that matters is the one that accumulates rather than
the one that is smallest at a single step.

A symplectic integrator preserves the phase-space volume of the flow it
approximates. The consequence, from backward error analysis, is that it solves a
nearby Hamiltonian problem exactly rather than the intended problem
approximately: there is a modified energy, differing from the true one by a
quantity of order `h^p`, that the numerical trajectory conserves to all orders in
the interval. The measured energy error therefore oscillates over each orbit and
returns to where it was, indefinitely. A non-symplectic method has no such
conserved quantity available to it, and its energy error accumulates with the
number of steps taken, without bound.

Two further properties of the gravitational problem sharpen the argument. The
cost of a step is entirely the force evaluation, so a method is compared not at
equal timestep but at equal evaluations: velocity Verlet spends one, RK4 four.
And the standard practice of the field, in stellar dynamics and in molecular
dynamics alike, has been leapfrog and its relatives for decades, for exactly
these reasons.

## Decision

Orrery's default integrator is velocity Verlet, second order and symplectic. A
fourth-order symplectic composition, Yoshida's, is available for runs that need
more accuracy per unit of simulated time and can afford three evaluations a step.
Classical RK4 is implemented and kept, but as a reference and a counterexample
rather than as a candidate for a production run.

The validation suite demonstrates the difference rather than asserting it. The
same eccentric two-body orbit is integrated by all three methods over four
hundred orbits at two hundred steps each, and the measured relative energy error
is reported as an envelope over the first and last twentieth of the run. Velocity
Verlet holds 2.6894e-3 at both ends, agreeing to six digits between them, Yoshida
holds 2.0170e-5 the same way, and RK4 grows from 1.92e-5 to 3.72e-4, a factor of
nineteen, while spending four force evaluations a step to do it. By the end of
the run the fourth-order non-symplectic method is eighteen times less accurate in
energy than the fourth-order symplectic one that cost a quarter less.

## Alternatives considered

**Default to RK4 because it is fourth order.** The choice a reader coming from
general-purpose numerical integration would expect. It is more accurate over a
short interval, and if the deliverable were one orbit computed once it would be
the right answer. It loses on the deliverable this project actually has: a long
run whose energy is the thing being reported. It also costs four force
evaluations per step, so it is compared against velocity Verlet at four times the
work and against Yoshida at a third more.

**Default to Yoshida's fourth-order composition.** A serious option, and the
reason it is implemented rather than described. It is symplectic, so it keeps the
bounded envelope, and its envelope is two orders narrower at the same timestep.
Against it: three force evaluations a step, and negative substeps that make the
intermediate states meaningless to anything sampling mid-step. Velocity Verlet
remains the default because for a large-N run the timestep is usually set by the
close encounters rather than by the accuracy target, and at that timestep the
extra evaluations buy accuracy the run did not ask for. The choice is a run-time
one, so a study that wants the accuracy can have it.

**Adaptive or individual timesteps.** The standard answer in collisional stellar
dynamics, and genuinely better for clusters where the range of orbital times is
wide. It is also where symplecticity is lost, since a step size that depends on
the state destroys the property this decision is built on, and recovering it
needs a time-symmetric or time-transformed scheme that is a project in itself.
Out of scope, and the softening in ADR-0008 already bounds the accelerations that
would otherwise force it.

**Implement only the symplectic methods and describe RK4 in prose.** Less code,
and the plan permits nothing here that is not needed. It gives up the comparison,
which is the point: a claim that symplectic integration matters is worth more when
the repository contains the measurement that shows it, made with the same
diagnostics, on the same configuration, in the same test run.

## Consequences

The project ships a method it recommends against, and has to say clearly in the
code why it is there. The header for RK4 does that, and the class reports
`is_symplectic()` as false so that nothing selects it by accident.

The energy behaviour test is a long-running one by the standards of a unit test:
eighty thousand steps for each of three methods. It takes about two seconds in an
optimised build and about ten times that in a debug build, which is acceptable
now and is the sort of test that later phases will want to keep out of the
inner development loop.

Every accuracy claim the project makes from here is qualified by an integrator
and a step count, and the comparison between methods is quoted per force
evaluation rather than per step. `Integrator::force_evaluations_per_step` exists
so that a benchmark cannot get that wrong quietly.

The bounded-versus-secular result is the project's first piece of evidence that
its physics is right for a reason rather than by coincidence, and it is reused in
the validation report of Phase 14.
