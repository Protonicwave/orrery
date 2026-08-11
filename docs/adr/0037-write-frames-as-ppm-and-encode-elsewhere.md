# ADR-0037: Write frames as PPM and leave the encoding to an external tool

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Phase 12 asks for offline frame export and a documented path to an encoded
video. The frames have to be written in some format, and the video has to be
made by something.

## Decision

Frames are written as binary PPM, three uncompressed bytes per pixel behind a
fifteen-byte ASCII header. The video is made by pointing ffmpeg at the
directory, and `docs/visualisation.md` gives the command.

## Alternatives considered

**PNG.** The obvious choice, and it would produce files around a fifth of the
size. It needs deflate, which means either a dependency, zlib or libpng or
stb_image_write, or several hundred lines of bit manipulation written here.
Neither is worth it for what these files are. A frame directory exists for the
seconds between the export finishing and the encoder reading it, and is then
deleted; the artefact that survives is the video. Paying a dependency to make a
temporary file smaller is the wrong trade, and every image viewer and every
encoder reads PPM already.

The one case where the size matters is a long export at high resolution: a
thousand frames at 1920 by 1080 is six gigabytes rather than one. That is a real
cost on a laptop, and `docs/visualisation.md` says so and says what to do about
it, which is to encode and delete in batches or to export at the resolution the
video will be.

**Encoding the video in the project.** Rejected without much hesitation.
Linking libavcodec, or any encoder, to turn a sequence of frames into an MP4
would be the largest dependency in the project by a wide margin, would need
configuration for pixel formats, rate control and containers, and would
reimplement the interface of a tool everybody already has. It would also put the
project in the business of tracking a codec library's API changes, which has
nothing to do with gravity.

**BMP or TGA.** Also uncompressed and also dependency-free, and both carry more
header than PPM for no benefit. PPM's header is human-readable, which means a
malformed frame can be diagnosed with `head -c 20`.

**Writing the frames to standard output for a pipe into the encoder.** This is
the right answer to the size problem: ffmpeg reads a stream of concatenated PPM
images, so the frames would never touch the disc at all. It is not done here for
one specific reason. A standard output stream on Windows is opened in text mode,
where a byte that happens to be a line feed is written as two, so piping binary
through it needs `_setmode`, which is a Windows-only call taking a file
descriptor, in a project whose only other platform-specific code is a CPU
topology query. That is a poor trade for a convenience, and the alternative that
costs nothing is to export at the resolution the video will actually be encoded
at rather than at the largest the machine can draw.

It is worth revisiting if a long export at high resolution becomes routine
rather than occasional.

## Consequences

No dependency, and a writer that is fifteen lines.

A frame directory is large, six gigabytes for a thousand frames at 1920 by 1080.
The documentation says so and says what to do about it.

The project cannot produce a video by itself. `docs/visualisation.md` states the
ffmpeg command and the version it was run with, and the README's claim about the
video is a claim about that documented command rather than about a program in
this repository.

The tests can read a written frame back and check its header and its bytes
without a decoder, which is a small benefit of an uncompressed format and is what
`tests/viz/image_test.cpp` does.
