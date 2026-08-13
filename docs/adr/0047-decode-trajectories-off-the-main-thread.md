# ADR-0047: Decode trajectories off the main thread

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The published gallery run is a trajectory of four hundred and one frames of
eight thousand particles, thirty-nine megabytes, and the client has to turn it
into positions a graphics device can draw. Turning one frame into three
`Float32Array`s means walking ninety-six kilobytes, checking a checksum over
every byte of it and converting each value, which is a few hundred microseconds.
There are four hundred such frames.

A browser has one thread that runs the page, and that thread is also running the
render loop. Doing the decode there costs a few hundred microseconds at a moment
when the frame budget is sixteen milliseconds, four hundred times, spread
through the first part of a session. That is four hundred frames dropped or
lengthened while the run loads, which does not read as a page that is loading:
it reads as an instrument that stutters.

The download has the same shape. Waiting for the whole file before drawing
anything means a black plate for as long as the connection takes, and the
connection is not the one the client was built on.

## Decision

Everything between the network and a `Float32Array` happens in a Worker:
`web/src/trajectory/worker.ts`, with the format reader and the ranged walk
beneath it. The page starts it, receives frames, and keeps them. It decodes
nothing.

Frames leave the Worker by transfer rather than by copy. A frame crosses the
boundary by having the ownership of its three buffers moved, so the message
costs the same whatever the particle count. That is the same intent as
ADR-0027's staged allocations on a different device: the data is put where it is
needed rather than duplicated on the way.

The file is read in ranged requests covering several frames at a time, and each
frame is handed on as soon as it has been decoded and checked, so playback
starts long before the file lands. Requests cover several frames rather than one
because a request has a cost that has nothing to do with how many bytes it
carries: at ninety-six kilobytes a frame, asking for one at a time would spend
more of the download on round trips than on positions.

The page keeps every frame it is sent. Thirty-nine megabytes of `Float32Array`
is memory a browser has, and holding it means that playing the run a second time
costs no network at all and that moving the transport to any instant is a
lookup. Dropping frames to save it would trade a resource the browser has for
one it does not.

A browser with no Worker is told so and shown nothing, rather than being given
the decode on the thread that is also drawing.

## The format needs no index, and does not get one

The trajectory format carries no frame count and no index, for the reason
`docs/formats/trajectory.md` gives: the header is written before the run has
taken a step, so a count in it would describe frames that a killed run never
wrote.

It was expected that a reader over a network would therefore need a sidecar
index published beside the trajectory. It does not, and writing one would have
been writing down something the file already says. Every frame is the same
length, that length follows from the particle count and the flags, and the
header's length follows from the same two. So the number of frames is the file's
length divided by the frame length, and the offset of frame `n` is a
multiplication. The file's length arrives in the `Content-Range` header of the
first ranged request, which had to be made anyway to read the header.

The division is floored, which drops a final frame that stops in the middle.
That is what the per-frame checksums exist to make possible, and it is the
behaviour a run cut off by a full disc should have.

What the client does keep beside the gallery is not an index but a statement of
which configuration each published run came from and which settings it changed,
in `web/src/gallery/runs.ts`. That is knowledge the trajectory genuinely does
not hold, it is read by the tool that publishes the run and by the client that
plays it, and it is therefore written once.

## Alternatives considered

**Decode on the main thread, in small pieces.** Slicing the work so that no
single piece exceeds a frame budget is a real technique and it would have
avoided a Worker. It also means the decode is interleaved with drawing, so the
frame rate falls while the run loads instead of being unaffected, and the
slicing has to be tuned against a budget that changes with the particle count.

**Fetch the whole file, then decode.** Simpler in both halves: one request, one
pass. It also means nothing on screen until the last byte arrives, which on a
slow connection is the difference between an instrument that is loading and one
that appears broken.

**A streaming fetch rather than ranged requests.** One request, decoded as the
body arrives, and it would work. Ranged requests were chosen because the same
mechanism serves the thing that comes next: a transport that can be moved to any
instant needs to be able to ask for a frame in the middle of a file it has not
finished reading.

**`SharedArrayBuffer`, with the Worker writing into memory the page can see.**
The fastest arrangement and unavailable here: it needs cross-origin isolation,
which needs the COOP and COEP response headers, which GitHub Pages does not set.
The same constraint decides the threading of the WebAssembly build.

## Consequences

The reader is a set of functions from bytes to numbers with no I/O in it, and a
source interface with two implementations, one over `fetch` and one over an
`ArrayBuffer`. That is what lets `web/tests/trajectory/` check it against
trajectories the C++ actually wrote, with no server started and no `fetch`
mocked. Two fixtures are committed for it, one from a single-precision build and
one from a double-precision build, against the rule that trajectories are not
committed: they are a few hundred bytes each, they are the only thing that says
this reader and `src/sim/trajectory.cpp` agree, and a test whose input has to be
regenerated by a toolchain nobody installed to work on the client is a test that
stops being run.

The page is told that more of the run has arrived a hundred times over a load
rather than four hundred, because a progress readout has a hundred distinct
values and four hundred re-renders to move it would put the cost back on the
thread this decision took it off.

The Worker is a second entry point in the build, which is a second file to keep
inside the size budget. It is five kilobytes.
