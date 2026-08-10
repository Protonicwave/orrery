# ADR-0024: Make quadrupole moments an option that is off by default

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

A cell stands in for its contents through a truncated expansion of its
potential. Keeping only the first term treats the cell as a point mass at its
centre of mass. Keeping the second adds the traceless quadrupole moment, which
describes how the cell's mass is spread about that point, and reduces the error
of an accepted cell by roughly a factor of `s / d` at every distance.

The literature is divided on whether it is worth it, and the division is not
about physics. Both sides agree the term is correct and that it improves the
answer at a fixed opening angle. The disagreement is about whether the same
accuracy is cheaper to buy by opening the angle instead, and that is a question
about the machine: the quadrupole makes each accepted cell more expensive, and a
smaller angle makes there be more of them.

## Decision

Quadrupole moments are computed and used when `TreeParameters::quadrupole` is
set, and not otherwise. The default is off. The moments live in an array
parallel to the nodes rather than inside them, and that array has no elements
when the option is off.

## Alternatives considered

**Always compute them.** The choice of the codes that keep them, and it would
simplify this project: one code path, one node layout, no parameter. The cost is
paid by every configuration that does not want them, and it is not small. A
quadrupole is six scalars against the seven that describe everything else about
a node, so carrying it inside the node nearly doubles the bytes a traversal
streams through the cache, and the traversal is the part of the evaluation this
project cares about. That is why the moments are in an array of their own even
when they are switched on: an evaluation without them touches no quadrupole
memory at all rather than skipping over it.

**Never compute them.** The choice of the codes that argue the angle is the
better knob. It would leave the project unable to answer the question, which is
the objection: the whole point of Phase 8 is an error against cost curve, and a
curve with one of the two available parameters missing is an argument rather
than a measurement. The comparison is now in
`docs/performance/barnes_hut.md`, made on this machine with these kernels, and
it can be read by anyone who disagrees with the conclusion.

**Higher orders.** Octupole and beyond. Each is another factor of `s / d` and a
considerably larger tensor: ten independent components at third order against
five at second, with a contraction to match. The second order is where the ratio
of accuracy to complexity is best for a criterion of this kind, and a project
that could not demonstrate the second being worthwhile has no business
implementing the third.

## Consequences

There are two configurations of the solver to test rather than one, and the
tests cover both: `tests/solvers/octree_test.cpp` checks the moment against an
independent calculation over every particle, and
`tests/solvers/tree_walk_test.cpp` checks the expansion against the exact
acceleration of a configuration whose expansion is known in closed form. The
second is the test that pins down the sign and the coefficients, and it was
written first for that reason: a quadrupole term with the wrong sign is of the
right size and makes the answer worse, which no test comparing two tree
configurations against each other would catch.

A tree with the moments switched on uses about twice the memory per node and
takes longer to build, since the moments are accumulated up the tree as well as
computed at the leaves.

The tensor is stored as six components although five determine it, since it is
traceless. That redundancy is deliberate and local: the traversal contracts the
tensor with a vector twice per accepted cell, and recovering the sixth component
from the other two would put an addition in the hottest loop of the solver to
save eight bytes in an array that only exists when the option is on. A test
asserts the trace is zero, so the redundancy cannot drift into an
inconsistency.

The softening applied to the quadrupole term is the same substitution the
monopole term uses, which is exact for the monopole and an approximation for
this one. The approximation is not measurable in the regime it is used: a cell
is only accepted when the target is several cell widths away, and the softening
length is a fraction of the smallest cell, so the correction to the correction
is of order the square of a small number times a term that is already small.
