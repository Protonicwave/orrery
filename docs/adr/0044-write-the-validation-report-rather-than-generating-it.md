# ADR-0044: Write the validation report rather than generating it

- **Status:** Accepted
- **Date:** 2026-08-12

## Context

The validation report is the document this project exists to be able to write.
It gathers every analytic comparison, convergence study and conservation result
into one place, and a reviewer's first question about it is whether it describes
the code as it is now or as it was when somebody last edited the file.

That question has an obvious answer available: generate the report from the test
suite. Catch2 emits XML, the cases already carry category tags, and the measured
quantities the report quotes are computed inside the tests that assert them.

## Decision

The report is written by hand. Each claim names the test that enforces it and
quotes the figure measured on the machine recorded in the performance report,
and the tests remain the authority on whether the claim still holds.

## Alternatives considered

**Generate it from Catch2's XML output.** What that produces is a list of case
names and pass or fail beside each. The suite's names are unusually descriptive,
so the list would read tolerably, and it would be honest about what ran. It would
also contain none of the content that makes the report worth reading: why a
tolerance is 0.10 and not 0.01, what a wrong velocity distribution would do to
the virial ratio instead, why the precession is attributed to the integrator
rather than to the force law, and which of two similar-looking results is the one
that means something. A test asserts that a number is inside a bound. Only prose
can say why that bound is the interesting one.

**Have the tests emit their measured values, and substitute them into the
document at build time.** This is the version that nearly works, and it fails on
what it would produce. The figures would then be those of whichever machine last
ran the suite, in whichever precision it was configured for, so the document
would say something different to every reader and would be comparable to nothing.
The figures quoted here are from one recorded machine on purpose, exactly as the
performance figures are.

**Publish no report, and let the test suite speak for itself.** Three hundred
case names in eight executables is not evidence a reader can weigh, and the
project's first stated goal is correctness that can be *demonstrated*. A
demonstration nobody can follow is not one.

## Consequences

The report can go stale, and the mitigation is structural rather than hopeful:
every figure it quotes is bounded by an assertion in the suite, so a change that
moved a number far enough to matter fails a test before it reaches the document.
The looser bounds are deliberate and are explained where they appear, since a
bound pushed against the last digit of one machine's arithmetic would be a claim
about that machine.

When a figure does change, the document has to be edited by hand. That is a real
cost and it is paid once per change to the physics, which is the right frequency
for re-reading a page that says what the project has proved.

The same reasoning covers the performance report, which quotes sessions rather
than regenerating them, and for the stronger reason that its numbers cannot be
reproduced by continuous integration at all: no hosted runner is a Lunar Lake
laptop with an Arc 130V in it.
