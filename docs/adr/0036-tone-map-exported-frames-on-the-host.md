# ADR-0036: Tone map exported frames on the host

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

The renderer accumulates light into a floating-point target and then compresses
its range with a curve so that it can be shown on a display. That curve has to
be applied somewhere.

For the window there is no question: it is a fragment shader over the whole
frame, which costs nothing and is where such things belong. For an exported
frame there is a choice, because the frame has to come back to the host either
way and the only question is whether it comes back already compressed to eight
bits per channel or as the floating-point radiance it was accumulated as.

## Decision

The window is tone mapped in a fragment shader. An exported frame is read back
as floating-point radiance and tone mapped on the host by `viz/tone_map.hpp`.
The curve is written twice, in C++ and in GLSL, adjacent in that one file.

## Alternatives considered

**Read back the tone mapped eight-bit frame.** Simpler, moves a quarter of the
bytes, and has the appealing property that what is exported is exactly what is
in the window down to the last bit. It was rejected on reproducibility. An
exported frame is a deliverable: the video in the README is made of these, and
someone should be able to regenerate it. A fragment shader's arithmetic is the
driver's: the precision of its intermediate values, the rounding of its
transcendental functions and the exact behaviour of its sRGB write are all
within the tolerances the specification allows to vary between vendors and
between driver releases. Doing the last step on the host means the same
accumulated buffer gives the same PNG-equivalent bytes on any machine that can
run the accumulation.

It also loses information that the export path can use. A frame read back as
radiance can be re-exposed without re-rendering, which is what somebody grading
a sequence actually wants. That is not implemented today and the format of the
readback is what would make it possible.

**Tone map on the host for the window as well.** Would remove the duplication
entirely, and would mean reading a full frame back from the device every time
one is displayed. A synchronous readback stalls the pipeline; measured on this
machine it is around twenty milliseconds at 1920 by 1080, which turns a renderer
that draws a million particles at a hundred frames a second into one that draws
anything at fifty.

**Tone map on the device for both, and read back the eight-bit result for the
export.** This is the first alternative again, and it is what the project would
do if the exported frames were only a preview.

## Consequences

The curve exists in two spellings, and nothing enforces that they agree. That is
the price, and it is stated plainly rather than argued away. It is mitigated by
keeping both in `viz/tone_map.hpp`, adjacent, with the GLSL returned as a string
that the renderer's fragment shader is built from, so the two are read together
and neither is somewhere a reader would not think to look. The C++ one is the
definition and is what the tests hold to its properties.

The encoding for display is not duplicated. The interactive path writes linear
values into an sRGB framebuffer and lets the hardware apply the transfer
function; the export path calls `encode_srgb`, which implements the same
standard function. That is one function specified by IEC 61966-2-1 applied in
two places, not two decisions.

An export costs a full-frame synchronous readback, which is what makes exporting
slower than drawing. Measured at 1280 by 720 on this machine, playing a
trajectory to the screen runs at 447 frames a second and exporting the same
frames runs at 61, and the difference is the readback and the file.
