# ADR-0032: Store the whole state in a checkpoint, accelerations included

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Section 7 of the implementation plan requires that a long run can be interrupted
and resumed to bitwise-identical state. The state of a run in this project is
the ten component arrays of `ParticleData` plus the step count, and one of those
ten is not independent: the accelerations are a function of the positions and
the masses, computed by the solver.

The integrators require that `accelerations()` holds the acceleration at
`positions()` on entry to every step and restore it on exit, which ADR-0013
records. A resumed run therefore has to start with the accelerations its
predecessor ended with, however it comes by them.

There are two ways to come by them: store them, or recompute them from the
positions with the same solver.

## Decision

The checkpoint stores all ten arrays, accelerations included, as exact bit
patterns. `Simulation::restore` takes a flag saying whether the accelerations
given are already the accelerations at those positions, and the resume path sets
it, so a restored state is installed rather than reconstructed.

## Alternatives considered

**Recompute the accelerations on resume.** The smaller file, by three tenths,
and it would work today. Every solver in this project is a deterministic
function of the positions and masses: the direct solver sums each target in
index order whichever worker computes it, which
`tests/solvers/parallel_direct_solver_test.cpp` asserts for equality rather than
against a tolerance, and the tree solvers build a tree that depends only on the
sorted particles. So recomputing would give the same bits.

The objection is that this is a property of the solvers this project happens to
have, and the requirement is about the checkpoint. A solver that reduced across
threads in completion order, or a GPU kernel whose work-group scheduling varied
between launches, would break bitwise resume in a way that no test of the
checkpoint format would catch and that would appear as a slow divergence
thousands of steps later. Storing the arrays makes the resumed state a copy of
the stored state, which is a statement about this file rather than about
whatever solvers exist when it is read.

It also costs a force evaluation on every resume, which for a two-million
particle tree on this machine is most of a second, and for the direct solver at
that size is minutes.

**Store the accelerations and verify them on resume** by recomputing and
comparing. This has some appeal: it would turn a solver that had lost
determinism into a loud failure at the point of resume. It was rejected because
it pays the evaluation this decision exists to avoid and then makes a resume
fail for a reason that is not a defect in the checkpoint. Solver determinism is
tested where solvers are tested.

**Store a trajectory frame and resume from that instead**, so there is one
format rather than two. ADR-0033 covers that and rejects it.

## Consequences

A checkpoint is about eighty bytes a particle in double precision, against
fifty-six if the accelerations were left out. For the largest configuration this
machine can integrate, two million particles, that is 160 MB against 112 MB.
Checkpoints overwrite one path rather than accumulating, so that is a bound on
what a run costs in disc rather than a rate.

`tests/sim/checkpoint_test.cpp` writes accelerations no solver would produce and
requires them back unchanged, which is what distinguishes a stored acceleration
from a recomputed one that happened to agree.

The resume path has to construct the simulation with no particles and then
restore, because the constructor establishes the acceleration invariant by
evaluating the field, which would overwrite what the checkpoint carried. That is
slightly awkward and is commented at both call sites. The alternative, a
constructor flag, would put a rarely used parameter on the class every test
constructs.
