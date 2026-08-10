# ADR-0020: Keep the arithmetic IEEE and measure what vectorising changed

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Section 8 of the implementation plan lists precision loss under fast
floating-point flags as one of the project's four standing risks, and requires
that any relaxation of IEEE semantics for the sake of vectorisation be a measured
decision validated against a compensated-summation reference rather than an
assumption. Phase 7 is where that becomes live.

The direct kernel's inner loop is a sum. Written scalar and in index order, it
is a serial dependency chain: each addition needs the result of the one before,
so the loop cannot use more than one of the machine's floating-point units at a
time however wide the registers are. That is the single biggest obstacle to
making it fast, and every route around it changes the arithmetic.

`-ffast-math` and its parts are the standard way through. They permit the
compiler to reassociate the sum, contract multiplies and adds, replace a
division by a reciprocal multiply, assume no value is a NaN or an infinity, and
flush subnormals to zero. Several of those would help this kernel substantially.
They are also global, invisible at the call site, and different on each of the
three compilers the project supports.

There is a second reason to be careful here that is specific to this project.
The direct solver in double precision is the reference every later result is
measured against, and energy conservation over long integrations is one of the
two headline validation results. Both are claims about small differences between
large quantities, and both are exactly what a relaxed floating-point mode
degrades first and most quietly.

## Decision

No fast-math flag is set anywhere in this project, in any preset, for any
target. Every kernel computes in IEEE-754 arithmetic with correctly rounded
operations.

Two departures from the scalar kernel's rounding are made deliberately in the
vector kernel and are named in `solvers/direct_kernel.hpp`. The sum is
reassociated across lanes: each lane keeps its own partial sum and they are
added at the end in a fixed, documented order. And the accumulation uses fused
multiply-add, which is an IEEE-754 operation with its own correct rounding
rather than a relaxation of the standard.

Both are validated against a compensated-summation reference in
`solvers/reference_kernel.hpp`, which computes the same physics in double
precision whatever scalar type the build selected and accumulates with Neumaier
compensation so that its own summation error is negligible against what is being
measured. `tests/solvers/kernel_accuracy_test.cpp` holds the measurement.

## Alternatives considered

**`-ffast-math`, or `/fp:fast`.** The whole bundle, globally. It is rejected on
what it includes rather than on what it enables: assuming no NaN and no infinity
disables the one behaviour this project relies on to notice a division by zero
in an unsoftened configuration, and flushing subnormals to zero changes the
result of a close approach in the single-precision build in a way no test would
attribute correctly. It is also not one decision but six, of which this kernel
wanted two.

**`-ffp-contract=fast` alone, letting the compiler introduce FMA.** Nearly
harmless, since FMA is more accurate than the separate operations it replaces,
and it is what GCC does by default in C++ anyway. Rejected as a project-wide
setting because it makes the scalar kernel's arithmetic depend on the compiler
and its version, and the scalar kernel is the reference: two machines running
the same build of the same source should agree on what the reference says. The
vector kernel uses FMA by writing the FMA intrinsic, where it is visible in the
source and appears in the file's documentation.

**Reassociation by the compiler, with `-fassociative-math`.** This is the flag
that would let the auto-vectoriser do to the scalar loop what the vector kernel
does by hand. Rejected because it is invisible. Under the flag the reference and
the fast kernel would both be reassociated, in a way neither file states, in a
pattern chosen by the optimiser and liable to change with its version. Written
by hand, the reassociation is four partial sums in a documented order, it is in
one file, and its effect is a number in a test.

**A reciprocal square root approximation.** `_mm256_rsqrt_ps` and one or two
Newton-Raphson refinements are the standard way to make an inverse-square-law
kernel fast in single precision, and this kernel spends much of its time in the
square root and the division it feeds. It is not adopted in this phase, and it
is not rejected on principle: it is a genuine relaxation, it would need its own
accuracy measurement against the reference this ADR establishes, and it belongs
with the single-precision GPU kernel of Phase 9 where the accuracy question is
already being asked. The instrument for judging it exists now, which is the
point of building it here.

**Compensated summation in the kernel itself.** Accurate, and roughly four times
the arithmetic per pair for an accuracy nothing in this project needs. The
compensated sum is the reference precisely because it is too slow to be the
kernel.

## Consequences

The kernel is slower than it could be. The division and the square root are not
approximated and the compiler is not permitted to reassociate anything, so some
of the arithmetic throughput of the part is left on the table. What that costs is
measured rather than argued about: `docs/performance/roofline.md` states the
kernel's achieved fraction of the measured floating-point ceiling, and the gap
between them is partly this decision.

In exchange, every number this project reports is reproducible. The same source
built by any of the three supported compilers produces the same answer from the
reference kernel, and the vector kernel's departure from it is two named
transformations with a measured size rather than an unbounded licence granted to
an optimiser.

The measurement is now a permanent part of the suite rather than a thing done
once. A future kernel, on the GPU in Phase 9 or in the tree walk of Phase 8,
inherits the reference and the test around it, and has to state its own error
against the same instrument.

The two kernels disagree in the last bits, which means the project can no longer
say that any two runs agree bit for bit without saying which kernel each used.
It can still say something stronger than a tolerance for each of them
separately: a given kernel is bit-for-bit reproducible across runs, thread
counts and chunk orders, which is what `tests/solvers/parallel_direct_solver_test.cpp`
and `tests/solvers/direct_kernel_test.cpp` assert.
