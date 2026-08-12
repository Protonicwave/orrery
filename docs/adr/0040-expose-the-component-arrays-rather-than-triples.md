# ADR-0040: Expose the component arrays rather than arrays of triples

- **Status:** Accepted
- **Date:** 2026-08-12

## Context

Almost every N-body package a Python user has met presents positions as an
`(N, 3)` array. It is the shape matplotlib wants, the shape `numpy.linalg.norm`
wants along an axis, and the shape somebody writing a script will reach for
without thinking.

This project does not store positions that way. ADR-0004 keeps one contiguous
array per component, because the force kernel reads x, y, z and mass and never
touches velocity, and because the x coordinates of consecutive particles being
adjacent is what lets a vector load take four or eight of them at once. That
decision is measured: it is worth a factor of 3.34 on one core in the AVX2
kernel and it is the reason the kernel runs at four fifths of the only hardware
ceiling it can compete for.

So an `(N, 3)` array of positions exists nowhere in memory, and the binding has
to decide what to do about that.

## Decision

`ParticleData` exposes ten NumPy arrays, one per component array, named
`position_x` through `mass`. Each is a one-dimensional view sharing memory with
the store. There is no `(N, 3)` property.

The `(N, 3)` shape is available as `orrery.stacked(particles)`, a free function
whose docstring says in its first line that it copies.

## Alternatives considered

**An `(N, 3)` property that copies.** The obvious convenience, and it was
rejected because of what it does on assignment. `particles.positions[0] = ...`
would look exactly like the ten arrays' behaviour, would run without error, and
would write into a temporary that is discarded on the next line. A silent
no-op is a worse interface than an absent one, and it would be the single most
likely thing a new user tried.

**An `(N, 3)` property returning a read-only copy.** This fixes the silent
no-op by making the write raise, at the cost of making the arrays asymmetric:
some of them can be assigned into and one cannot, for a reason that is a detail
of the storage rather than of the physics. It also still copies on every access,
which for a plotting loop over a million particles is the expense the phase
exists to avoid.

**Change the storage to an array of `Vec3`.** This is the alternative a reviewer
might most reasonably expect, since it makes the Python interface natural at a
stroke. It is refused for the reason ADR-0004 gives at length, and refusing it
here is the correct direction of dependency: the binding is a consumer of the
library, and a layout chosen for a 135 GB/s memory bus is not renegotiated by a
convenience in one of its callers.

**A structured NumPy dtype with named fields.** A view of the ten arrays as one
record array is not possible either, since a structured array requires the
fields of one element to be adjacent and here they are ten arrays apart.

## Consequences

The interface says what the storage is. A person reading `position_x` learns
something true about why this project is fast, which is not the worst thing an
interface can teach.

Code that wants triples writes `orrery.stacked(...)` and pays for a copy at a
place where the copy is visible. Code that wants speed uses the components,
which is also the form that vectorises in NumPy: `x * x + y * y + z * z` over
three contiguous arrays is faster than the same reduction over an `(N, 3)`
array's last axis.

`orrery.components(particles)` returns the three views as a tuple for unpacking,
so `x, y, z = orrery.components(state)` is the idiom, and it copies nothing.

A future phase that added a second storage layout would have to revisit this.
Nothing here assumes there is only one, but nothing here abstracts over the
possibility of two either.
