# The instrument

The browser client: what it draws, where the run it draws comes from, and what
the drawing costs.

[The instrument](https://protonicwave.github.io/orrery/instrument/) is published
beside this documentation. It reads a trajectory this repository produced and
draws it with the same optics as the native renderer, on whichever of the two
graphics interfaces a browser offers.

## What it draws

A published run of `examples/collision.orrery`: the two disc galaxies on a
bound, grazing encounter that `docs/visualisation.md` describes, integrated
through the whole encounter and the merger.

It is a smaller sample than the configuration file's own, and the settings it
changes are stated on the plate rather than left to be discovered. Eight
thousand particles instead of sixty thousand, and a frame every hundred steps
instead of every forty:

```
orrery run examples/collision.orrery \
    --set initial_conditions.count=8000 --set run.steps=40000 \
    --set output.trajectory_stride=100 --set output.diagnostics_stride=400
```

Both changes are about the download rather than about the physics. A frame is
three component arrays, so the particle count is the size of every frame, and at
sixty thousand particles the same run would be a gigabyte and a half.

That run took 338 seconds, 8.44 ms per step, and conserved energy to 7.1 parts
in a thousand across forty thousand steps. Its virial ratio went from 0.942 to
0.934. It was written by the `single-precision` preset, on the machine
[the performance report](performance.md) describes.

The trajectory is 38 536 056 bytes: a header of 32 036 and 401 frames of 96 020.
Single precision for the same reason as the smaller count, and at no cost to the
picture: the renderer converts to single on the way to the device whatever the
file holds, so the other half of a double-precision file would be bytes fetched
in order to be discarded.

The client states the count it actually drew, in the corner of the plate beside
the name of the device that drew it. A picture drawn in a browser is not one of
the native figures and must not be readable as one.

## Publishing a run

The gallery is generated, not committed. A trajectory is reproducible from a
configuration file and a seed, which is the rule `.gitignore` already states for
`*.otj`, and the site workflow runs the simulator before it builds the client.
By hand, from `web/`:

```
npm run gallery -- ../build/single-precision/apps/orrery
```

The runs and the settings each one changes are defined in
`web/src/gallery/runs.ts`, which is the only place they are written down: the
tool reads it to decide what to run, and the client reads it to describe what it
is playing. The tool refuses a binary of the wrong precision, because that
mistake produces a working run of twice the size and nothing else would catch
it.

## Reading a trajectory in a browser

The reader is in `web/src/trajectory/`, written against
[the format specification](formats/trajectory.md) rather than against the C++
that writes it, and checked against files the C++ wrote.

It runs in a Worker, and everything between the network and a `Float32Array`
happens there: the ranged requests, the checksums, and the conversion of a
component array to the precision the device draws in. ADR-0047 records why.
Frames leave the Worker by transfer rather than by copy.

The file is read in ranged requests of five frames each, eighty-one of them for
the published run, and each frame is drawn as soon as it has been decoded and
checked. Playback therefore starts while the rest of the run is still arriving,
and the plate says how much of it has been read.

The format carries no frame index and needs none. Frames are all the same
length, so the number of them is the file's length divided by that length and
the offset of any one of them is a multiplication; the file's length arrives in
the `Content-Range` header of the first request, which had to be made anyway to
read the header. `docs/formats/trajectory.md` sets out the arithmetic.

## Drawing it

Two backends behind one interface, WebGPU first and WebGL2 second, and an honest
sentence if neither will start. ADR-0046 records why, and what the two do
differently.

The picture is the native renderer's: every particle as a sprite with a Gaussian
falloff, additively blended into a half-float target with no depth test, then
one pass applying the extended Reinhard curve of
`include/orrery/viz/tone_map.hpp`. A galaxy is not a set of opaque objects, and
what a picture of one should show is the sum of light along each line of sight.

The controls are the native viewer's, with the same sensitivities from
`src/viz/viewer_window.cpp`, so a drag of a given fraction of the window turns
both by the same angle. Drag to turn, right-drag or shift-drag to pan, scroll to
zoom, `-` and `=` for the exposure, `r` to reframe, space to play and pause.
Each has a keyboard equivalent: the arrow keys turn, Page Up and Page Down zoom.

The plate carries the furniture that does measurement work. A scale bar in model
units, with `G = 1` beside it because a length in these units means nothing
without the unit system named (ADR-0007). A gnomon showing where the model's
axes point after a drag has lost them. The run's name, its configuration, its
seed, the exposure in stops, the particle count and the device.

Playback runs at sixty trajectory frames per second of wall clock rather than
one frame per refresh, so the run takes the same seven seconds on any panel.
Nothing plays until it is asked to.

## What it costs

Measured on the machine [the performance report](performance.md) describes, in
Chromium, with the plate at 1004 by 514 device pixels and the published run's
eight thousand particles. The tool is `web/tools/measure_render.ts`:

```
npm run build
npm run preview &
npm run measure -- webgpu
```

| Backend | Frames in 60 s | Frames per second |
| --- | --- | --- |
| WebGPU | 3601 | 60.0 |
| WebGL2 | 3601 | 60.0 |

Sixty is the refresh rate, so both mean that no frame was missed in a minute.
The figures are from a machine with nothing else drawing on it; a second browser
still shutting down was enough to halve one of them.

The other half of the measurement is the heap, because a render loop that
allocates a little on every frame looks exactly like one that does not until it
has been running for a while. The whole run is read first, so what is being
looked for is growth after the thirty-nine megabytes of positions are all
present and the loop is drawing frames it already has.

| What is running | Heap growth per frame |
| --- | --- |
| An empty animation-frame callback, for comparison | 1.3 bytes |
| The render loop, with the transport paused | 18 to 21 bytes |
| The render loop, playing | 124 to 125 bytes |

Reading down that table: the loop's own drawing costs about seventeen bytes a
frame above an empty callback, which is the browser marshalling the calls rather
than anything the renderer allocates, and it is the same on both backends to
within two bytes. The remaining hundred is not the loop at all. It is the clock
and the step readout, which are sampled from the loop ten times a second and
rendered by React, six hundred times in the minute. Pausing the transport stops
the instant changing and therefore stops those renders, while the loop goes on
measuring, uploading, drawing and tone mapping every frame.

## Where the pieces are

| What | Where |
| --- | --- |
| The format, read | `web/src/trajectory/format.ts` |
| The ranged walk over a file | `web/src/trajectory/reader.ts` |
| The Worker, and what it says to the page | `web/src/trajectory/worker.ts` |
| The renderer interface | `web/src/render/renderer.ts` |
| The two backends | `web/src/render/webgpu.ts`, `webgl2.ts` |
| The camera, ported from `viz/camera.hpp` | `web/src/render/camera.ts` |
| The render loop and the controls | `web/src/render/instrument.ts` |
| The scale bar and the gnomon | `web/src/render/furniture.ts` |
| The published runs | `web/src/gallery/runs.ts` |
