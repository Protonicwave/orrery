# ADR-0030: Send the node array to the device narrowed rather than transposed

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Section 7 of the implementation plan asks Phase 10 for a GPU-suitable tree
representation, which invites the assumption that the Phase 8 tree is not one.
It is worth being exact about how much of the work was already done.

`solvers/octree.hpp` stores the nodes in a single array, in the order a
depth-first walk meets them, with the first child of a node immediately after it
and an index just past each node's subtree recorded in the node itself. There
are no pointers anywhere and a traversal keeps no stack. Every one of those
properties was chosen for the CPU and every one of them is worth more on a
device: a stack per work-item is private memory, which on this hardware spills
to global memory and is the usual reason a first GPU tree walk is slow, and a
child pointer is a dependent load whose address no lane can predict.

So the question is not how to restructure the tree. It is whether anything about
the node itself should change on the way across, and the conventional answer is
yes: transpose it. ADR-0004 stores particle components in separate arrays
because the force kernel reads positions and masses and never touches
velocities, and the same reasoning applied mechanically to nodes would give one
array per field.

That reasoning does not survive contact with what the traversal actually does.
ADR-0029 has the whole sub-group at one node at a time, so a node is read once
and broadcast to 32 lanes rather than gathered across 32 addresses. Every field
of that node is read: its centre of mass and acceptance radius decide the test,
its mass or its particle range is used whichever way the test goes, and one of
its two index fields says where to go next. Held as separate arrays, that one
node would be five reads from five places.

`octree.hpp` makes the same argument for the CPU walk and draws the same
conclusion, noting that ADR-0004's real decision was to follow the access
pattern rather than to prefer separate arrays.

## Decision

The host node array is copied to the device unchanged in structure, one struct
per node in depth-first order, and narrowed: the three index fields become
32-bit where `core::Index` is 64.

The quadrupole moments stay in a separate array, exactly as Phase 8 keeps them,
and are staged only when they are switched on.

## Alternatives considered

**Transpose the nodes into component arrays.** The conventional GPU layout and
what a reviewer would expect from ADR-0004. Rejected because a coherent
traversal reads one node at a time for the whole sub-group, so the access
pattern is the opposite of the particle kernel's: everything about one record,
rather than one field of many records. Under the independent traversal the lanes
read different nodes, but each still reads all of one, so the transpose would not
help there either.

**Keep 64-bit indices and copy the node verbatim.** Simplest, and it removes the
conversion loop and the bound below. Rejected on size. In a single-precision
build a node with 64-bit indices is 48 bytes and a node with 32-bit indices is
32, which is exactly half a cache line, so two nodes share a line and the array
the walk streams through is a third smaller. `core::Index` is `std::size_t`
because that is what the host's containers report, which is a statement about
addressable memory rather than about how many nodes a tree has.

**Have the octree build directly into shared memory.** Would remove the
conversion entirely. Rejected on layering, which is the same objection ADR-0027
raises against allocating the particle arrays in unified memory: `solvers/` must
build a tree in a configuration with no GPU, no runtime and no device, and every
CPU test and CPU run would otherwise carry a device dependency to serve a
backend that is off by default.

**Pack the leaf range into the escape index, since a node needs one or the
other.** An internal node has no particle range and a leaf's escape index is
always the node after it, so three 32-bit fields could be two. Rejected because
it saves eight bytes of a 32-byte node at the cost of a branch before every
field access in the hottest loop in the solver, and because a node that means
different things depending on a flag is exactly the representation that makes a
traversal hard to read.

## Consequences

The device cannot address more than 2^32 - 1 particles or nodes. That bound is
unreachable on this machine by three orders of magnitude, since four billion
particles would be 144 GB of component arrays on a part with 32 GB, but it is
checked at the evaluation boundary rather than assumed, because the failure it
would otherwise produce is silently wrong accelerations rather than a
diagnostic.

The conversion is O(number of nodes), which is a fraction of N set by the leaf
capacity, and `SyclTreeTimings::node_staging` reports it separately so that the
fraction can be read rather than argued about.

The tree the device walks and the tree the host built are the same tree, node for
node and index for index, which is what lets a test require the two solvers'
interaction counters to be equal. A transposed or repacked representation would
have made that comparison an argument about whether the transformation preserved
meaning.
