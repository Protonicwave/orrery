# ADR-0029: Mask the accepted cells rather than descending together

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

A GPU executes work-items in sub-groups of a fixed width, 32 on the target
device, which share one instruction pointer. When the lanes of a sub-group take
different branches the hardware executes both sides and masks off the lanes that
did not want each. A tree walk written as though each work-item were an
independent thread therefore does not cost what its own node count suggests: the
lanes are already stepping through the union of their walks, with most of them
switched off for most of it.

The established remedy is to stop pretending and have the sub-group walk
together. One node index for the whole group, advanced by agreement, so that the
node is read once for the group, the acceptance test is one instruction over 32
targets, and the loop has a single instruction stream rather than 32 interleaved
ones.

The published warp-coherent traversals then make a choice that this project
cannot make. Having voted that some lane needs to open a cell, they open it for
every lane, including the lanes that had decided the cell was far enough away to
stand in for its contents. Those lanes descend anyway and sum the cell's
children, or its particles, individually.

That is not an error and it is easy to see why it is attractive. Opening a cell
that could have been accepted makes the answer *more* accurate, not less, and it
removes the need for any per-lane state at all: every lane does the same thing at
every node, which is the purest possible form of the mitigation.

What it costs is the identity of the answer. Under that scheme, the acceleration
of a particle depends on which other particles happened to share its sub-group,
because they decide which cells get opened on its behalf. Change the sub-group
width, change the device, change the particle count enough to shift the padding,
and the answer changes.

This project cannot afford that. `solvers/tree_walk.hpp` states as a guarantee
that the order of summation depends only on the tree; the direct solver is kept
as the reference precisely so that every approximation can be measured against
it; and section 3 of the implementation plan permits several backends of one
solver but not two solvers computing different physics.

## Decision

The sub-group walks together, and a lane that accepts a cell records the index
its own walk would have jumped to and contributes nothing until the group
reaches it.

The group advances to the smallest index any lane still wants, so the sequence
of nodes it visits is exactly the union of its lanes' individual walks. Each
target is therefore summed over exactly the nodes its own walk would have
visited, in the same order.

## Alternatives considered

**Descend wherever any lane must, and let the accepting lanes descend too.** The
published scheme, described above. Rejected because the result depends on the
sub-group composition, which makes it incomparable with the CPU solver, with
itself at a different width, and with the direct reference this project measures
everything against. It is also, and this was not obvious in advance, slower: a
lane that descends where it could have accepted goes on to sum the cell's
descendants, so the scheme buys its extra accuracy with extra arithmetic that
masking does not perform. The choice made here costs nothing in speed and the
alternative would have had to be justified on accuracy grounds it cannot claim,
since accuracy that varies with the hardware's SIMD width is not accuracy anyone
can quote.

**Vote on whether to descend, and otherwise follow the current node's escape
index.** One ballot instruction per node rather than a minimum reduction across
the sub-group, so cheaper per node visited. Rejected because it visits nodes
inside subtrees that every lane has already accepted: the group cannot tell that
it is inside one until it reaches the end. The minimum is taken instead, which
makes the visited sequence exactly the union and makes `node_visits` report a
quantity that means something rather than an implementation artefact.

**Coherence at the work-group rather than the sub-group.** More targets agreeing
to walk together, so each node is read for 256 lanes rather than 32. Rejected
because there is no divergence between sub-groups to remove: they have separate
instruction pointers and are free to be at different nodes, so wider coherence
adds redundant visits without saving anything. It would also need a barrier and
a shared node index per node visited, where the sub-group cooperates through
registers.

**A per-lane stack, and no coherence at all.** The traversal the CPU walk
replaced with an escape index. Rejected for the reason the escape index exists,
which is worse on a device than on a host: a stack per work-item is private
memory, and private memory on this hardware spills to global memory. It is the
usual reason a first GPU tree walk is slow.

## Consequences

The two device traversals are the same function of the input. That is what makes
the mitigation measurable as a pure performance change,
`tests/solvers/sycl_tree_solver_test.cpp` requires it, and
`docs/performance/sycl_tree.md` quotes a speedup that no accuracy difference is
hiding inside.

The interaction counters of the GPU solver equal the CPU solver's exactly, which
is a far stronger statement about a traversal than any tolerance on an
acceleration: it says the device opened the same cells and summed the same
pairs, rather than that it arrived somewhere nearby.

The cost is one register per lane for the skip index and one comparison per node
visited, and a sub-group reduction rather than a ballot at each step.

The redundancy is real and is reported rather than absorbed. A lane visits its
whole sub-group's union, so `node_visits` is larger for the coherent traversal
than for the independent one, and the ratio between them is the price of the
scheme. The performance document quotes both, because a mitigation that reported
only the number that improved would be advocacy rather than measurement.
