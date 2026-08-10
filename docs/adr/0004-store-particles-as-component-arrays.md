# ADR-0004: Store particles as one array per component

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

The target machine shares roughly 135 GB/s of memory bandwidth between its CPU
and its integrated GPU. A direct force evaluation performs a fixed amount of
arithmetic per pair of particles, and on this hardware the arithmetic is not
what limits it: the rate at which particle data can be brought into the cores
is. How the particles are laid out in memory therefore decides how fast the
kernel can possibly run, before a single line of the kernel is written.

The access pattern is asymmetric and known in advance. The force kernel reads
positions and masses for every particle it considers, writes accelerations once
per particle, and never reads velocities at all. The integrator reads and writes
positions and velocities and reads accelerations. Diagnostics read everything.
The kernel that runs N^2 times is the one whose layout matters.

A cache line is 64 bytes. In double precision a particle carries a position, a
velocity, an acceleration and a mass: 80 bytes in total, of which the force
kernel needs 32.

## Decision

Particle state is stored as ten separate contiguous arrays of `Real`, one per
scalar component: `position_x`, `position_y`, `position_z`, the same three for
velocity and acceleration, and `mass`. They are owned by `ParticleData`, which
keeps them all at the same length and exposes them as `std::span`, with
`Vec3Span` carrying the three components of a vector quantity together.

## Alternatives considered

**An array of particle structs.** The layout most readers would expect, and the
one that makes a single particle easy to talk about. It fails the only test that
matters here: fetching a particle's position also fetches its velocity and
acceleration, which the force kernel never reads, so more than half of every
cache line it pulls in is wasted. On a bandwidth-bound kernel that is close to a
factor of two in achievable performance, paid on every one of N^2 interactions.

**An array of `Vec3` per quantity.** Separating positions from velocities fixes
most of the waste above, and it keeps a position as one addressable object. It
gives up the second half of the argument: the x components of consecutive
particles are then 24 bytes apart rather than adjacent, so filling a 256-bit
register with four x coordinates needs a strided gather instead of one aligned
load. Phase 7 depends on those loads being contiguous.

**An array of structures of arrays, in blocks.** Blocking the components into
groups of the vector width is the layout that performs best in some published
N-body codes, and it is a plausible refinement of what is decided here. It is
not adopted now because its advantage over plain component arrays is a question
about this machine's cache behaviour that the project cannot yet answer:
Phase 7 builds the roofline analysis that would decide it. Adopting it now would
be a complication justified by a guess.

**Loose parallel arrays with no container.** Ten `std::vector` objects passed
around individually is what the decision above amounts to without the class.
Nothing then holds them at the same length, and a solver receiving nine of them
and a stale tenth is a bug with no natural place to be caught. The container
exists to make the lengths a class invariant.

## Consequences

Reading or writing one particle becomes a gather or a scatter across ten arrays
rather than a single memory access. `Vec3Span::get` and `set` provide it for
setup code, tests and diagnostics; using either inside a loop over particles
would give back exactly what the layout was chosen for, and that is the thing to
watch for in review.

The size invariant now needs enforcing. Every operation that changes a length
goes through one private helper in `ParticleData` that applies it to all ten
arrays, so an array cannot be missed, and an eleventh quantity added later is a
change at the one place that lists them.

Adding a per-particle quantity is more work than adding a field to a struct. In
exchange, a quantity that a hot kernel does not read costs that kernel nothing
at all, which is the property this project is optimising for.

The layout is the same one the SYCL backend in Phase 9 wants. A GPU work-item
reading `position_x[i]` for consecutive `i` produces a coalesced access without
any rearrangement, so the CPU and GPU backends can share one container rather
than each keeping its own copy in its preferred order.
