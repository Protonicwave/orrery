# ADR-0014: Give the force solvers an interface of their own above the acceleration field

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

ADR-0012 gave the integrators an abstract acceleration field: masses and positions
in, accelerations out, and nothing else. It is deliberately narrow, because that
is the whole of what an integrator needs to know about gravity, and a wider
interface there would let an integrator depend on facts about solvers that some
future solver would not have.

Phase 5 adds the first implementation of that interface, and with it a set of
questions that nothing in the integrators layer asks but that everything else
does:

- A benchmark comparing two solvers has to name them in its output.
- A validation test comparing an approximate solver against direct summation has
  to check that both soften the same way, because otherwise the difference it
  measures is the softening rather than the approximation.
- A diagnostic measuring the potential energy of a configuration needs the
  softening the force kernel used, for the reason ADR-0008 exists.
- A performance report has to state the work a solver did in a unit that is a
  property of the algorithm rather than of the machine, which means counting
  interactions.

None of that belongs on the interface the integrators see, and all of it is
common to every solver this project will contain.

## Decision

The solvers layer declares `ForceSolver`, which derives from
`integrators::AccelerationField` and adds four members: `name`, `softening`,
`interaction_count` and `reset_interaction_count`. A solver is passed to an
integrator as an acceleration field and to everything else as a force solver.

## Alternatives considered

**Use `AccelerationField` directly and add nothing.** The simplest option, and it
works right up to the first benchmark. At that point the caller either downcasts,
which is the interface admitting it was the wrong one, or holds solvers by their
concrete types, which gives up the run-time selection from a command-line flag
that section 3 of the implementation plan requires.

**Put the four members on `AccelerationField` instead.** One interface rather
than two. It makes the integrators' contract carry three concepts it cannot use
and one it must not: a test that integrates under an analytic field, a uniform
field or a harmonic well, has no softening and no interactions to count, and
would have to implement both to say so. The narrowness of that interface is what
ADR-0012 bought and this would spend it.

**Compose rather than inherit: a solver that holds an acceleration field.** It
would allow a solver to swap its own kernel at run time. The relationship is a
genuine is-a, since computing accelerations is not something a solver does
alongside its real job but is the job, and the wrapper would add an indirection
per force evaluation and a second object to keep alive for no capability the
project needs.

**Template the callers on the solver type and drop the virtual call.** No
dispatch at all, and every call site inlinable. It gives up the run-time
selection, multiplies the compile time of every test and benchmark by the number
of solvers, and buys an indirect call that happens once per force evaluation
ahead of N(N-1) interactions. The plan permits dispatch at exactly this boundary
because the measurement is unarguable.

**Return the interaction count from `evaluate` rather than accumulating it.** No
mutable state in the solver and no reset to remember. It changes the signature the
integrators see, so that every acceleration field would have to return a count,
which is the second alternative above by another route.

**Count interactions in a global or a static counter.** It would avoid the
accessors entirely. Two solvers in one process would then share a counter, which
the accuracy-against-cost study of Phase 8 needs not to be true, and the threading
of Phase 6 would make it a data race.

## Consequences

Every solver implements four members beyond the kernel, three of which are one
line each. A solver that forgot to maintain its counter would report having done
no work, which is the kind of thing that stays wrong for a long time, so the
count is asserted against the closed form in a test rather than trusted.

The interaction counter is mutable state on the solver, which is why `evaluate`
is not const. ADR-0012 anticipated that and gave the reason there.

The counter is updated once per evaluation rather than once per interaction, so
Phase 6 can thread the kernel without an atomic in the inner loop or a per-thread
partial count. Phase 8's tree walk has no closed form for its work and will have
to accumulate per thread and sum at the end, which is a cost of that phase rather
than of this interface.

Reporting the softening as part of the interface means a caller can no longer
soften differently from the solver by accident. It can still do so deliberately by
passing a different value to a diagnostic, which is a thing a test occasionally
wants, so the accessor is a way to be right rather than a guarantee of being
right.
