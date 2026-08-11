# ADR-0033: Keep the trajectory and the checkpoint apart

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Phase 11 asks for two binary outputs: a compact trajectory format for the
recorded path of a run, and checkpoint and restart. Both are sequences of
particle components written to a file, and the second is very nearly a special
case of the first. It is natural to ask whether one format could do both, with a
trajectory being a checkpoint stream and a checkpoint being the last frame of
one.

## Decision

Two formats, with different magic, different headers and separate readers and
writers. `docs/formats/trajectory.md` and `docs/formats/checkpoint.md` specify
them.

## Alternatives considered

**One format, with the trajectory as a lossy setting of it.** The unification is
real but the two files are wanted for opposite reasons, and every property that
serves one hurts the other.

A trajectory is read by something that is not this program: a renderer in Phase
12, a Python analysis in Phase 13, a plotting script. It wants to be small,
because it holds hundreds of frames, so it holds positions alone by default and
no accelerations at all. It wants to survive an interrupted run, so it has no
frame count in its header and a checksum on each frame rather than on the file.
It wants its masses once, in the header, because they do not change.

A checkpoint is read by this program and nothing else. It wants to be complete
rather than small, holding all ten arrays exactly for the reasons ADR-0032 gives.
It wants to be replaced atomically, which a file that is appended to cannot be.
It wants the configuration inside it, so that resuming needs the file alone. It
wants to be refused outright when damaged, where a trajectory wants to give up
every frame it can.

Combining them means a format with flags saying which of its parts are present,
a reader that decides at run time whether the file it has can be resumed from,
and a person who discovers after an eight-hour run that the file they kept was
written without velocities. The saving would be one small reader and one small
writer, both of which are thin over `sim/binary_stream.hpp` already.

**Checkpoint by keeping the last two trajectory frames.** Attractive because it
removes a format entirely, and wrong for a reason that has nothing to do with
size: a trajectory frame is written at the stride the person chose for looking at
the run, which for a long run is every few thousand steps, and it does not carry
the accelerations. A resume would be from whenever the renderer wanted a picture,
and would not be exact.

**One file holding both, with the checkpoint at the end.** Rejected on the
atomicity argument alone. A checkpoint is replaced by a rename, and a file that
is also being appended to cannot be replaced without losing the appended part.

## Consequences

Two specifications to maintain, two readers, two writers and two sets of tests,
which is the cost. It is smaller than it looks because both are written in terms
of the same fixed-width binary primitives, and neither reader is longer than a
page.

A run may write both, and the validator refuses a configuration that points them
at the same path, since whichever wrote second would produce a file neither
reader accepts.

`orrery inspect` decides which format a file is from the first eight bytes rather
than from its name, so a person who has named a checkpoint `.otj` still gets a
useful answer.

The trajectory cannot be resumed from, and that is worth stating plainly in its
own header comment because someone will otherwise try. A run that wants both a
picture and a resumable state writes both files.
