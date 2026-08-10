# ADR-0015: Compute every pair from both ends rather than applying Newton's third law

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The force between two particles is equal and opposite. A direct summation can
therefore visit each unordered pair once, compute the interaction, and add it to
one particle while subtracting it from the other. That halves the arithmetic:
N(N-1)/2 interactions instead of N(N-1). It is the first optimisation anyone
suggests for an N-body kernel, it is correct, and it is a factor of two on the
operation that is essentially the whole run time of this project.

The direct solver written in Phase 5 does not do it, and a reviewer would
reasonably expect an explanation.

## Decision

Each particle's acceleration is computed by a loop that reads every other
particle and writes nothing but its own accumulator. Every pair is evaluated
twice, once from each end, and the project reports N(N-1) interactions per
evaluation rather than quoting the pair count.

## Alternatives considered

**Symmetric accumulation over unordered pairs.** The factor of two, at three
costs that are each larger than it on this machine.

The first is memory traffic, which section 2 of the implementation plan
identifies as the binding constraint at roughly 135 GB/s shared with the
integrated GPU. The form used here writes the acceleration arrays N times per
evaluation, once per particle, and reads the position and mass arrays in long
contiguous sweeps. The symmetric form writes them N(N-1)/2 times, because every
interaction updates the accelerations of both participants, and the second update
lands at an index the loop does not control. Halving the arithmetic while
multiplying the writes by N is the wrong trade in the direction that matters
here, and it gets worse as N grows past the point where the acceleration arrays
fit in the 8 MB of L3.

The second is threading, which arrives in Phase 6. Partitioning the ordered form
by i gives each thread a private range of the output and no synchronisation at
all. Partitioning the symmetric form by pair gives two threads a shared
destination, which needs atomics in the innermost loop, a private acceleration
array per thread costing 24 bytes per particle per thread to allocate and a
reduction to combine, or a colouring of the pairs that constrains the schedule
just where Phase 6 needs it free to balance four performance cores against four
efficiency cores.

The third is vectorisation, which arrives in Phase 7. The ordered form's inner
loop reads eight consecutive j at a time and accumulates into a register, which
is what AVX2 does well. The symmetric form has to write eight results back to
eight non-consecutive addresses, and AVX2 has no scatter instruction: that
capability arrives with AVX-512, which this part does not have. The eight stores
would be done one at a time, and the factor of two would be spent several times
over.

**Symmetric accumulation with blocking.** Tiling the interaction matrix so that a
block of j accumulations stays in registers recovers some of the locality, and
serious codes do it. It also makes the reference implementation of the project's
reference algorithm substantially harder to read, at a phase whose stated purpose
is correctness, in exchange for at most a factor of two. The asymptotic win this
project is actually after is Phase 8's tree, which is a factor of N/log N and
does not care how the leaves are summed.

**Compute the ordered form now and switch later.** This is the decision, stated as
a plan rather than as a choice. It is written down as a choice because the
alternative is the one that gets retrofitted quietly during a performance phase,
after which the thing being optimised is no longer the thing that was validated.

## Consequences

The direct solver does twice the arithmetic it strictly needs to, and every
performance figure the project quotes for it is affected. This is reported rather
than absorbed: the interaction counter records N(N-1), the roofline analysis in
Phase 7 places the kernel against the hardware limits with that count, and the
achieved fraction is therefore of a bound the kernel could in principle double.
Reporting it the other way, quoting N(N-1)/2 pairs and dividing, would flatter the
result by exactly the factor this decision gave up.

Interaction counts quoted here are twice the figure a paper counting unordered
pairs would give for the same problem. That is stated wherever the number appears
so that a comparison against published work is not off by two in a direction
nobody notices.

Conservation of linear momentum becomes a property worth testing rather than an
identity. Symmetric accumulation makes the total momentum change zero by
construction, to the last bit, whatever the force law is: a kernel with the wrong
exponent, the wrong softening or the wrong sign would still conserve momentum
exactly. Here the cancellation happens only because the two halves independently
compute the same magnitude from the same masses and the same separation, so the
property test in `tests/solvers/conservation_test.cpp` is evidence about the
kernel rather than about the loop that was written to guarantee it.
