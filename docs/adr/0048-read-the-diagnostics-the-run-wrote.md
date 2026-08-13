# ADR-0048: Read the diagnostics the run wrote

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The instrument's data rail plots what a run conserved: its energy drift, its
virial ratio, its angular momentum and its linear momentum, against the same
time cursor the plate and the transport read. Those four numbers already exist
twice over. The simulator measures them with `core/diagnostics.cpp` and writes
them to a CSV file beside the trajectory, at whatever stride the configuration
asks for. The client could also work them out for itself: a trajectory carries
the masses in its header, so a kinetic energy needs only the velocities and a
potential energy needs only a sum over pairs.

Two sources for one number is the situation the repository has a rule about, and
the rule does not say which of them to keep. It says to keep one.

## Decision

The client reads the diagnostics file. It computes no conserved quantity of its
own, and `web/src/diagnostics/series.ts` is a parser and nothing else.

The reason is not that the arithmetic would be hard. It is that the two answers
would differ, and the difference would be the browser's rather than the run's.
The published trajectories are written in single precision (ADR-0006 makes that
a property of the build) and at a stride of a hundred steps, so a browser
recomputing an energy would be summing a rounded copy of a sampled state with
none of the compensation `core/diagnostics.cpp` applies. The rail exists to say
what the run conserved. A number computed in the browser answers a different
question, and answering it under the same label is the failure mode the whole
data rail is there to avoid.

There is a second reason and it is the stronger one for the tree runs. A
potential energy is a sum over pairs, which is `N(N−1)/2` terms; at eight
thousand particles that is thirty-two million evaluations for one point on one
plot, four hundred times over. The run has already paid that cost once.

The consequence of not recomputing is that the rail can only plot what the file
carries, and the file carries no step time. `sim/diagnostics_log.cpp` writes the
step, the time, the three energies, the relative energy error, the virial ratio
and the two momentum vectors, all of which are properties of the state. How long
a step took is a property of the machine, and the driver reports it once at the
end of a run rather than per sample. So the fourth plot is the linear momentum
and not the step time: it is a column the file has, and it is the one that says
most, because a total momentum is the cancellation of `N` terms of both signs
and staying at round-off is the strongest conservation statement the run makes.
The measured step time keeps its place in the rail's register of measurements,
beside the particle count and the step count it belongs to.

## Alternatives considered

**Compute the diagnostics in the browser from the trajectory.** One file to
fetch instead of two, and it would let the rail show a quantity at every frame
rather than at every diagnostics sample. It also produces a number that is not
the run's, at a cost that grows as the square of the particle count, and it puts
a second implementation of the project's conservation arithmetic in a second
language where nothing compares the two.

**Carry the diagnostics inside the trajectory.** It would make a run one file
and remove the possibility of the two disagreeing about which run they describe.
It also changes a format that is specified, tested and already written by
something else, in order to save a fetch of fifteen kilobytes, and it would put
measurements that exist at one stride inside a file written at another.

**Fetch the diagnostics in the trajectory's Worker.** Consistent with ADR-0047,
and unnecessary. That decision is about tens of megabytes decoded frame by frame
while the loop is drawing. A diagnostics file is a hundred lines, parsed once,
before anything is drawn from it.

## Consequences

The reader is strict in the way the configuration reader is strict. It finds
columns by name rather than by position, so a column inserted in the C++ cannot
silently shift what a plot is showing, and it refuses a file with a missing
column, a short row or an unparseable field rather than reading the part it
understands. A run killed while writing its last line is exactly the run someone
will want to look at, and half a sample plotted as a sample is a measurement of
something that did not happen.

Every figure in the rail's register of measurements now comes from the run being
shown rather than from a run the documentation describes. The energy drift and
the virial ratio are the last and the first rows of the file the plate is
playing beside, so the register and the plots cannot disagree.
