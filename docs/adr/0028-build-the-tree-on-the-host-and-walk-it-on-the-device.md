# ADR-0028: Build the tree on the host and walk it on the device

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

A Barnes-Hut evaluation is four steps: sort the particles along a space-filling
curve, build an octree over the sorted order, gather the positions and masses
into that order, and walk the tree once per particle. Phase 10 moves the solver
to the GPU, and the question it cannot avoid is which of the four go with it.

The literature's answer is all of them. The published GPU Barnes-Hut
implementations build the tree on the device, because on a discrete card the
alternative is a host-to-device transfer of the whole node array on every
timestep, and that transfer is expensive enough to justify a second
implementation of tree construction written in terms of atomics.

Neither half of that argument holds here. Section 2 of the implementation plan
records that this machine's GPU shares its memory controller with the CPU and
that there is no bus for a transfer to cross, and ADR-0027 records the
measurement behind the staging step that remains. So the reason to move
construction to the device is absent, and what is left is the question of where
the time actually goes.

Phase 8 measured that on the CPU: the traversal dominates, and the sort and the
build together are a minority of an evaluation. The traversal is also the step
whose cost is irregular, divergent and hard, which is precisely what section 7
of the implementation plan identifies as the interesting part of this phase.

## Decision

The Morton ordering, the octree construction and the moment aggregation stay on
the host, in the code Phase 8 already wrote and Phase 8's tests already cover.
Only the traversal moves to the device.

The gather that puts the particles into the tree's order writes straight into
shared unified memory, so it is simultaneously the reordering the algorithm
needs and the staging the device needs, and the scatter back out reads from the
same place.

## Alternatives considered

**Build the tree on the device as well.** The conventional answer, and the one a
reviewer coming from the CUDA literature would expect. Rejected on three
grounds. It would be a second implementation of octree construction, and section
3 of the implementation plan names two divergent implementations of the same
physics as the standard way projects of this kind decay; the correctness
argument for the existing one rests on tests that a device implementation would
not inherit. It would buy a fraction of an evaluation, at these sizes, of the
step that is not the bottleneck. And device-side construction is where the
subtle defects live: it needs atomics to assign particles to cells and a
device-wide synchronisation between levels, neither of which the traversal
needs, and both of which would be introduced to speed up something already
measured as minor.

**Move the sort to the device and leave the build on the host.** More
interesting than it sounds, and the measurement in
`docs/performance/sycl_tree.md` is what makes it so: the sort, not the build, is
the largest host cost, and it grows as the traversal shrinks. A device radix
sort over Morton codes is a well-understood kernel with no tree in it, so it
would not duplicate any physics. It is not done here because Phase 10 is one
coherent concern and that concern is the traversal; the measurement that would
justify it did not exist until this phase produced it. It is recorded as the
first thing to do to this solver rather than as a rejected option.

**Keep the tree between timesteps and update it.** Would remove construction
from most evaluations. Rejected by ADR-0022 already, for reasons that do not
change on a device: the positions have moved, and a tree that was updated rather
than rebuilt is a tree whose accuracy depends on how long ago it was built.

**Walk on the host and use the device for the leaf summations only.** The leaves
are the regular, vectorisable part, so this would put the easy half on the wide
machine. Rejected because the leaf ranges are a few dozen particles each and a
kernel launch per leaf is orders of magnitude more expensive than the summation;
batching them into one launch would mean building the batch, which is a
traversal.

## Consequences

The GPU solver is a backend of the Phase 8 solver in a strong sense: it produces
the same tree, from the same code, with the same parameters, so its accuracy is
Phase 8's accuracy and its interaction counts can be required to equal Phase 8's
exactly. `tests/solvers/sycl_tree_solver_test.cpp` requires all three.

The staging that ADR-0027 introduced costs this solver almost nothing, because
the copy it describes turns out to be the gather the algorithm already performed.
What remains is the node array, which is O(N / leaf capacity) rather than O(N),
and `SyclTreeTimings` reports it separately so that the claim can be checked.

What becomes harder is scaling. The device traversal gets faster with every
optimisation applied to it and the host half does not, so the share of an
evaluation spent on the host rises with N and eventually with effort. That is
the limit `docs/performance/sycl_tree.md` reports on rather than a limit this
decision hides: the largest tractable particle count on this machine is set by
host-side tree construction, and it is set there because of this decision.
