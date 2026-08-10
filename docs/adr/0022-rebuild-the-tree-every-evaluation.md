# ADR-0022: Rebuild the tree on every force evaluation

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The tree describes where the particles are. They move. A simulation asks for
accelerations once or twice per timestep and runs for millions of steps, and in
one step nothing moves far: a well-chosen timestep moves a particle a small
fraction of the softening length, which is itself a small fraction of the
smallest cell that holds more than a few particles.

So the tree built for step n is very nearly the tree wanted for step n+1, and
throwing it away looks wasteful. Codes that keep one exist, and the techniques
are well known: update the moments of every cell without touching the structure,
move the few particles that crossed a cell boundary, or rebuild only the
subtrees that changed.

## Decision

Everything is rebuilt on every force evaluation: the bounding cube, the Morton
codes, the sort, the node array and every moment.

## Alternatives considered

**Keep the structure and update the moments.** The cheapest of the three, and
the one that looks most obviously safe. It is not safe, and the reason is what
this decision turns on: the opening criterion is a statement about geometry, and
a cell that was small enough to accept at a distance is only still small enough
if the particles are still in it. A particle that drifted out of its cell is
being summed at a centre of mass it does not belong to, and the error is
unbounded rather than merely larger, because nothing in the criterion refers to
where the particle actually is any more. Worse, the error grows with the number
of steps since the last rebuild, so a run's accuracy would depend on a rebuild
interval that no measurement here bounds.

**Move the particles that crossed a boundary.** Correct, and considerably more
machinery: a particle has to be found in its old leaf, removed, inserted in the
new one, and the moments of every cell between the two updated. Cells become
empty and have to be pruned, cells overflow and have to be split, and the node
array stops being in depth-first order, which is the property the traversal in
`solvers/tree_walk.hpp` is built on. Recovering that order means compacting the
array, which is the rebuild this decision was avoiding.

**Rebuild the subtrees that changed.** The compromise, and it needs the same
bookkeeping as the previous option to work out which subtrees those are.

## Consequences

Construction is a cost every evaluation pays, and the argument for paying it has
to be a measurement rather than an assertion. It is in
`docs/performance/barnes_hut.md`, which reports the four phases of an evaluation
separately: the sort, the gather, the tree build and the traversal. The claim
this ADR is making is that the first three together are a small fraction of the
fourth, and the table is where that is checked.

The reason it can be a small fraction is structural rather than lucky. The
traversal is O(N log N) with a body of several tens of floating-point operations
per node visited, and it visits several hundred nodes per particle. The
construction is O(N log N) with a body of comparisons and moves. They differ by
the ratio of a softened inverse cube root to a 16-byte copy, and that is a large
number.

A rebuilt tree also has one property no incremental scheme can offer, and this
project needs it more than most: the tree is a pure function of the positions
and the parameters. Two evaluations of one configuration give one answer,
whatever happened before them, so the solver can be validated against direct
summation on a configuration handed to it cold and the result says something
about every evaluation rather than about the first one. The determinism tests in
`tests/solvers/barnes_hut_test.cpp` rest on it.

If a later phase finds construction dominating, the honest remedy is to make
construction faster rather than to make it rarer. Phase 10's GPU traversal will
change the ratio, since it makes the traversal much faster without making the
build faster, and this decision is worth revisiting there with the measurement
that phase produces rather than reversed in advance.
