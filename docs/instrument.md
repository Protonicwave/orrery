# The instrument

The browser client: what it draws, where the run it draws comes from, and what
the drawing costs.

[The instrument](https://protonicwave.github.io/orrery/instrument/) is published
beside this documentation. It reads a trajectory this repository produced and
draws it with the same optics as the native renderer, on whichever of the two
graphics interfaces a browser offers.

## What it draws

Three published runs, in the order the argument goes. Each is one of the
repository's own configurations with a few settings changed, and each states
what it changed and why beside itself.

| Run | Configuration | Bodies | Steps | Frames | Trajectory |
| --- | --- | --- | --- | --- | --- |
| Kepler | `examples/kepler.orrery` | 2 | 2 000 | 401 | 17 688 B |
| Cluster | `examples/cluster.orrery` | 4 096 | 20 000 | 401 | 19 734 392 B |
| Collision | `examples/collision.orrery` | 8 000 | 40 000 | 401 | 38 536 056 B |

**The two-body problem** first, because it is the only gravitational system with
a closed solution and therefore the only one whose picture can be checked by
looking at it. Ten revolutions of the eccentric orbit
[the validation report](validation.md) is built on, at forty frames each:

```
orrery run examples/kepler.orrery --set run.steps=2000 \
    --set output.trajectory_stride=5 --set output.diagnostics_stride=20
```

Its energy plot is the one to read. Velocity Verlet holds the error inside an
envelope rather than driving it one way, and over these ten orbits the envelope
is 1.5 parts in a thousand and is the shape of the orbit rather than a trend.
The run takes 0.055 seconds.

**A Plummer sphere** next, because it is an equilibrium the samplers are
validated against, so a correct sample stays in balance and the rail can be
watched saying so. Twenty time units, which is several crossing times:

```
orrery run examples/cluster.orrery --set run.steps=20000 \
    --set output.trajectory_stride=50 --set output.diagnostics_stride=200 \
    --set output.checkpoint_stride=0
```

Its virial ratio goes from 1.032 to 0.989 and stays within a few per cent of one
throughout, and its energy drift is 2.1 parts in a hundred thousand over twenty
thousand steps. The run takes 98 seconds, 4.89 ms per step.

**The collision** last, because it is the run the repository exists to draw: the
two disc galaxies on a bound, grazing encounter that
[the visualisation report](visualisation.md) describes, integrated through the
whole encounter and the merger. Eight thousand particles instead of sixty
thousand, and a frame every hundred steps instead of every forty:

```
orrery run examples/collision.orrery \
    --set initial_conditions.count=8000 --set run.steps=40000 \
    --set output.trajectory_stride=100 --set output.diagnostics_stride=400 \
    --set output.checkpoint_stride=0
```

Both changes are about the download rather than about the physics. A frame is
three component arrays, so the particle count is the size of every frame, and at
sixty thousand particles the same run would be a gigabyte and a half. That run
took 338 seconds, 8.46 ms per step, and conserved energy to 7.1 parts in a
thousand across forty thousand steps. Its virial ratio went from 0.942 to 0.934.

All three were written by the `single-precision` preset, on the machine
[the performance report](performance.md) describes. Single precision for the
same reason as the smaller count, and at no cost to the picture: the renderer
converts to single on the way to the device whatever the file holds, so the
other half of a double-precision file would be bytes fetched in order to be
discarded.

The client states the count it actually drew, in the corner of the plate beside
the name of the device that drew it. A picture drawn in a browser is not one of
the native figures and must not be readable as one.

## Where a run is

A run and a moment in it are both in the address, so both can be sent to
somebody: `?run=cluster&t=12.5` opens the Plummer sphere and puts the transport
at twelve and a half time units. ADR-0049 records why they are query parameters
and why nothing else in the interface is one.

The gallery's entries are ordinary links to those addresses. Choosing one is
intercepted so the client is not fetched again, and back and forward move
between runs.

The instrument goes to the frame nearest the moment asked for, because a
trajectory is written at a stride and the instants between two frames are
instants the run did not record. The clock then shows that frame's own time.
Moving the transport by hand rewrites the address; playback does not.

## What the rail plots

Four columns of the diagnostics file the run wrote, beside the run: the relative
energy error, the virial ratio, and the magnitudes of the total angular and
linear momenta. No gridlines, no legend, no axis furniture. The value above each
plot is the value at the instant the plate is showing, and the brass cursor
through all four is the transport's, so the plots and the picture are never
reading different moments.

The fourth plot is the linear momentum and not the step time, which the
diagnostics file does not carry: how long a step took is a property of the
machine rather than of the state, and the driver reports it once at the end of a
run. ADR-0048 records the decision to read that file rather than recompute its
columns in the browser, and the momentum is the better plot in any case, because
a total momentum is the cancellation of N terms of both signs and staying at
round-off is the strongest conservation statement a run makes.

Under them, when the console asks for it, is a radial mass profile. That one is
not a measurement the run made: it is worked out in the browser from the frame
on the plate and the masses in the trajectory's header, and it is labelled as
derived, because a plot in a rail of measurements reads as a measurement.

None of this renders while a run plays. The lines are drawn when the run changes
and when the box is resized; the cursors and the values follow a broadcast of
the instant, ten times a second, by writing into elements.

## What the console can and cannot do

The three tiers are the cost of operating them: instantaneous, derived from the
trajectory, and needing a new run. Every control that cannot act is drawn back
rather than removed, keeps its place in the tab order, and carries the reason at
the foot of its tier.

| Control | Tier | State |
| --- | --- | --- |
| Exposure, sprite radius | View | Live |
| Tone curve | View | Live, with the two curves the project has |
| Trails | View | Needs the renderer to accumulate more than one frame |
| Lab frame | View | Every run is already in the centre-of-mass frame |
| Bulge only | View | A frame holds positions, not which component a particle came from |
| Diagnostics, radial profile | Derived | Live |
| Overlay | Derived | An octree is the tree the solver built, which is not in the file |
| Rotating frame | Derived | Needs to know which galaxy each particle came from |
| Orbit trace | Derived | Needs several frames drawn into one picture |
| Bound / unbound | Derived | Needs velocities, which a published run is written without |
| Bodies, softening, integrator | Solver | Needs a run, which the service will provide |
| Run it in this browser | Solver | Live, at the size the WebAssembly build permits |

Read together, those reasons say what a trajectory is: positions and masses, at
a stride, and nothing else.

The last row is the exception, and it is a run rather than something derived
from one. It steps the scenario on the plate here, in a Worker, using the same
C++ compiled to WebAssembly, cut to what a tab can do: at most four thousand and
ninety-six particles and the first two thousand steps, both stated in the note
beside the control. While it runs the plate's catalogue carries the solver, the
kernel, the thread count, the measured step time and the energy error, all
reported by the module, so that a picture computed in a browser cannot be taken
for one of the figures in the performance report.
[`docs/webassembly.md`](webassembly.md) is that build in full.

The tone curve offers `reinhard`, which is the extended Reinhard curve of
`include/orrery/viz/tone_map.hpp` and what the native renderer draws with, and
`linear`, which is no curve at all. The second is there to be looked at rather
than used. The argument for tone mapping a galaxy is that the sum of light along
a line of sight through the middle of one is tens of times the sum through its
outskirts and a display has a ratio of about a hundred to work with; selecting
`linear` shows what that argument is about, which is a saturated core with no
structure in it and outskirts that have gone black. No third curve is offered
because the project has no third curve.

## Publishing a run

The gallery is generated, not committed. A trajectory is reproducible from a
configuration file and a seed, which is the rule `.gitignore` already states for
`*.otj`. Two jobs in continuous integration need the runs, the one that builds
the published site and the one that drives the client in a browser, and both
produce them through `.github/actions/gallery` so that a change to how a run is
made cannot reach one of them and not the other. By hand, from `web/`:

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
the collision, and each frame is drawn as soon as it has been decoded and
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

The four keys the native viewer reads from its window are read from the whole
page as well as from the plate. A browser has no such thing as the window having
focus, so space and `r` pressed by somebody who has not clicked the plate first
would otherwise do nothing at all. A key pressed inside a control still belongs
to that control.

The transport's track is a range input under its appearance, which is what gives
it the arrow keys, Home and End, a reported minimum and maximum and a spoken
value. It steps by one trajectory frame rather than by one integrator step, and
its ticks stand at the run's diagnostics stride, because that is where the data
the rail plots actually exists.

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

Everything added to the rail since is written into elements rather than
rendered: the four sparkline cursors, the four values above them and the radial
profile all follow the same ten-hertz sample by assignment. The render loop
itself is unchanged, which is why the table above still describes it. Take the
measurement again on a quiet machine before quoting a new figure; the tool says
what it needs.

## The editor beside it

The instrument plays runs this repository has already made.
[The editor](editor.md) is the page that makes one: the same design system, the
same solver in a Worker, and a scenario drawn as a technical drawing with its
lengths dimensioned rather than as a picture of the particles. What leaves it is
a configuration file the native binary runs unmodified, which is what makes the
client a way into the repository rather than only a window onto it. ADR-0053
records how it is drawn and why.

## The reading half

You observe in the dome and you read in the library. Beside the instrument, at
[`/instrument/method/`](https://protonicwave.github.io/orrery/instrument/method/),
are four pages of prose: the demonstration, the validation, the performance
report and the design of the solvers. They are the same type system at the
opposite value, sharing `tokens.css`, the three faces, the hairlines and the
masthead, so that arriving at one from the instrument is arriving at a different
page of one thing.

They are static HTML and they run no script (ADR-0050). A method page is markup
and one stylesheet, so it is readable before anything has executed and by a
reader who executes nothing; `npm run budget` fails if a built page acquires a
script tag.

The prose is written rather than generated from the Markdown in `docs/`. These
reports are the reference and are longer than a page on a site should be; the
site's version is shorter, arranged as an argument, and links back here for the
full tables. What holds the two together is that every figure on a page names
the file it was taken from:

```html
<span class="num" data-source="docs/performance.md">95.68</span>
```

`web/tests/method/figures.test.ts` opens each named file and requires the figure
to be in it, normalising the two differences that are typography rather than
value: the thin space these pages group digits with, and the minus sign they set
instead of a hyphen. A table names its source once and every figure cell in it
is checked. There are 163 such figures, and the test is what makes the pages
transcriptions rather than recollections.

The instrument links into the reading half from its masthead and from three
places in the rail, each to the page that argues for what is beside it. Those
addresses are in `web/src/method/links.ts`, and a test requires each to be a page
that exists and an entry of the build, since nothing imports a static page and a
renamed one would otherwise leave a dead link behind.

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
| The published runs, and the address | `web/src/gallery/` |
| The reading half | `web/method/` |
| Its stylesheet, and the paper palette | `web/src/styles/paper.css` |
| The diagnostics reader and its plots | `web/src/diagnostics/` |
| The radial profile, derived here | `web/src/diagnostics/profile.ts` |
