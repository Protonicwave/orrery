# ADR-0021: Sort a copy of the configuration rather than the caller's particles

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

A Barnes-Hut evaluation wants the particles in Morton order. The ordering buys
two things at once: the tree can be built from a sorted array with no insertion
pass, because every cell's particles are then contiguous, and the traversal
reads positions and masses sequentially because particles near each other in
space are near each other in memory.

The configuration does not arrive in that order. It arrives in whatever order
the initial-condition sampler produced, which has nothing to do with position,
and after a few thousand timesteps it is in an order that used to have something
to do with position and no longer does.

So something has to be permuted on every force evaluation. The question is what.

## Decision

`BarnesHutSolver` holds its own copy of the positions and masses in Morton
order, gathers into it at the start of each evaluation, and scatters the
accelerations back into the caller's order as they are computed. The caller's
arrays are read and never reordered.

## Alternatives considered

**Permute the caller's particle store.** The obvious alternative and the one
most published tree codes use, because it is free: the sort that was going to
happen anyway leaves the data where the next evaluation wants it, the gather
disappears, and after the first step the configuration is nearly sorted already,
so each subsequent sort is cheap.

It is rejected on the interface rather than on the arithmetic. A force solver in
this project is handed three spans and asked for accelerations
(`integrators/acceleration_field.hpp`), and the whole reason that interface is as
narrow as it is that an integrator can hold whatever storage it likes and swap
solvers without either of them knowing. A solver that reordered its inputs would
break that in ways which do not announce themselves: velocities live in a
container the solver was never given, so a solver permuting positions would
separate each particle from its own velocity; the Yoshida integrator holds
accelerations across substeps, and a permutation between two of them would add
one particle's acceleration to another's; and a caller holding an index to a
particle it cares about would find it pointing at a different particle after a
step, with no diagnostic.

Every one of those is fixable, and the fix is the same in each case: give the
solver the whole particle store rather than three spans, and have it permute all
ten arrays together. That is a considerably wider interface, adopted so that one
solver of several can avoid one linear pass, and it would put the reordering of
a simulation's state inside the object least entitled to do it.

**Keep an index array and read through it.** No copy and no reordering: the tree
stores particle indices and the traversal reads `positions.x[order[j]]`. This
does not work for the reason the sort exists. The leaves are summed by the direct
kernel of Phase 7, which loads four consecutive doubles at a time from each
component array; through an index array every one of those becomes a gather, and
AVX2 gathers from arbitrary addresses are several times slower than a contiguous
load on this part. The permutation would also be followed on every one of the
hundreds of leaf pairs each particle computes, rather than once.

**Sort the caller's arrays and put them back.** Permute in, evaluate, permute
out. This has all of the cost of the copy, twice, and mutates the caller's
storage in between, so a concurrent reader sees a configuration in an order
nobody asked for. It combines the disadvantages.

## Consequences

Every evaluation pays a gather of four arrays and a scatter of three, which is
seven linear passes over the configuration with no arithmetic in them. That is
O(N) against a traversal that is O(N log N) with a heavy body, and
`docs/performance/barnes_hut.md` reports it as a measured percentage rather than
an assumed one.

The solver holds four component arrays of its own, so a tree solver over N
particles costs 32N bytes in double precision beyond the configuration itself.
For the largest run this machine will do, that is a few tens of megabytes
against the configuration's own hundreds.

The sort cannot exploit the near-sortedness of a configuration that has only
moved a little since the last step, because the solver's copy is overwritten by
the gather before the sort sees it. A code that permuted in place would sort an
almost-sorted array each step and could use an algorithm that exploits it. This
is the one real cost of the decision, and it is bounded by what the sort is
measured to be: a single-figure percentage of a build, which is itself a small
fraction of an evaluation.

The direct solver and the tree solver take exactly the same arguments, so the
validation comparing them passes both the same spans and no permutation stands
between the two answers. That is worth something on its own: the project's
headline accuracy claim is a difference between two solvers, and the two would
be harder to trust if one of them had rearranged the configuration first.
