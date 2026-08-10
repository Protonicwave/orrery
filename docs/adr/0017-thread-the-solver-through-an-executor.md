# ADR-0017: Thread the solver through an executor it does not own

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Phase 6 had to make the direct solver run on eight threads. Section 3 of the
implementation plan is explicit that a faster implementation of the same physics
is a backend behind the existing solver interface and not a second solver, on
the grounds that two divergent implementations of the same equations is the
standard way projects of this kind decay. It does not say what the seam between
the two should look like.

The question is live because the obvious answer is the wrong one. A
`ParallelDirectSolver` deriving from `DirectSolver`, or beside it, would compile
today and be the natural place for someone to make a small optimisation to the
threaded path six months from now, at which point the validated kernel and the
one that runs are different code.

## Decision

`DirectSolver` holds a non-owning pointer to a `backend::Executor`, which is
null by default. The kernel body is written once, as a callable over a range of
target particles, and is either invoked directly on the calling thread or handed
to the executor. The executor divides `[0, N)` however it likes and knows
nothing about gravity.

## Alternatives considered

**A parallel solver class.** Rejected for the reason above: it makes the
threaded kernel a separate body of code, and nothing but discipline keeps it in
step with the reference. It also multiplies with every later phase, since a
Barnes-Hut solver and a SYCL solver would each need their own threaded twin.

**A template parameter on the solver.** `DirectSolver<Executor>` would remove
the indirect call and let the compiler inline the scheduler into the kernel. The
call it removes happens once per force evaluation, ahead of N(N-1) interactions,
and section 3 of the plan identifies exactly that boundary as where runtime
polymorphism belongs. In exchange it would make the solver type depend on the
scheduler type, so a benchmark comparing two schemes could not hold them in one
container, a command-line flag could not select one, and every header that
mentions a solver would need the scheduler's definition.

**Threading inside the solver.** Give `DirectSolver` its own thread pool and a
worker count. Simpler to call, and wrong on ownership: a pool of eight threads
is a machine-wide resource, one is enough for a whole simulation, and a solver
that owned one could not be constructed inside a loop without creating and
destroying eight threads each time round. It would also put the same scheduler
in the Barnes-Hut solver of Phase 8, and then a third copy in Phase 9.

**A global executor, or a shared default one.** A `serial_executor()` returning a
reference to a function-local static would remove the null check and let the
solver always call through the interface. It was drafted and rejected: the
executor accumulates statistics, so a shared default is global mutable state,
and two solvers on two threads would race over counters neither of them asked
for. A null pointer meaning "run on the calling thread" costs one predictable
branch per force evaluation and owns nothing.

**Putting the scheduler in `core`.** It would have saved a layer. The plan
already names `backend/` as a layer for exactly this, CPU threading now and SYCL
in Phase 9, and a scheduler on the include path of every file that wants a
3-vector is the same mistake ADR-0010 records for the Plummer sampler.

## Consequences

The solver has a reference to something it does not own, and the lifetime rule
that comes with it: the executor must outlive the solver. This is stated in the
header and is the normal shape for a shared resource, but it is a rule a caller
can break, which a value member would not have been.

The executor interface is narrow to the point of being inflexible. It divides a
range and runs a callable over the pieces. There is no task graph, no
dependency tracking, no nested spawning and no future to wait on, because no
kernel in this project needs any of them. A future phase that does need them
will have to widen the interface or add a second one, and widening it should be
resisted until something concrete requires it.

Every later solver gets threading for free, which is the point. Phase 8's tree
walk is a loop over particles whose bodies are independent, so it takes the same
executor without the scheduler learning anything about octrees, and Phase 9's
device backend slots in beside these three rather than inside them.

A task handed to an executor must not throw. Section 4 of the plan already
forbids an exception leaving a kernel; here the rule has teeth, because the task
runs on a worker thread where there is nothing to catch it. The worker entry
points are `noexcept`, so such an exception terminates at the point it was
thrown rather than corrupting the pool's bookkeeping on the way out.
