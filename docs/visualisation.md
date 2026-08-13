# Visualisation

How to look at a simulation, what the viewer costs, and how to turn a run into a
video.

Everything here needs a build configured with the renderer, which is off by
default because it fetches GLFW and needs an OpenGL 3.3 driver:

```
cmake --preset renderer
cmake --build --preset renderer
```

That produces `orrery-view` beside `orrery`. The simulator itself is unchanged
by the option; a build without it is a complete simulator without a viewer.

## Watching a run

```
orrery-view run examples/collision.orrery --set initial_conditions.count=20000
```

This assembles exactly the simulation `orrery run` would, from the same
configuration file, and draws each state as it is reached. Drag to turn, drag
with the right button to pan, scroll to zoom, `-` and `=` to change the exposure,
space to pause, `r` to reframe on the particles, `p` to write the current frame
to a file, escape to leave.

The camera frames the particles automatically at three times their
root-mean-square radius about the centre of mass. That is a compromise rather
than a bound: a bounding sphere would be set by whichever particle the encounter
flung furthest and would show a galaxy the size of a full stop. `--distance`,
`--azimuth` and `--elevation` place it explicitly.

A run advances one step per frame by default. The timestep a configuration needs
for the physics is usually far shorter than a frame wants, so a demonstration
raises `--steps-per-frame` until the motion is at a watchable speed.

## Playing back a trajectory

A run large enough to be worth looking at is usually too large to integrate at
interactive speed. The answer is to integrate it once, writing a trajectory, and
then play the trajectory as often as wanted:

```
orrery run examples/collision.orrery
orrery-view play collision.otj
```

Playback costs only the drawing, so it runs at whatever the renderer can manage,
and the camera can be moved freely while it plays.

A trajectory carries no configuration, so playback cannot tint the two galaxies
of a collision apart the way a live run does. That colouring is a property of
which galaxy a particle was sampled into, and a trajectory records positions.

## Making a video

Export the frames, then encode them:

```
orrery-view play collision.otj --export frames --width 1920 --height 1080
ffmpeg -framerate 60 -i frames/frame_%05d.ppm \
    -c:v libx264 -pix_fmt yuv420p -crf 18 collision.mp4
```

The export creates the window without showing it, so nothing appears on screen
and the machine is free to do something else. Frames are numbered from zero with
five digits, which is what lets the encoder be pointed at a pattern rather than
at a list.

`-pix_fmt yuv420p` is not optional in practice. Without it ffmpeg keeps the full
chroma resolution, which is better, and produces a file that several players and
most browsers will not open.

The frames are uncompressed, three bytes per pixel: about six megabytes each at
1920 by 1080, so six gigabytes for a thousand frames. ADR-0037 records why they
are PPM rather than PNG. For a long export, either encode and delete in batches
or export at the resolution the video will actually be, which is usually the
same thing.

`--spin` adds a fixed rotation to the camera between frames. A merger remnant is
a three-dimensional object and a video is not, so a turntable is the honest way
to show its shape:

```
orrery-view play collision.otj --export frames --spin 0.004
```

## The demonstration

`examples/collision.orrery` is the scenario: two disc galaxies of unequal mass on
a bound, grazing encounter, which fall together, pass, separate and return. The
initial conditions are described in
`include/orrery/initial_conditions/disc_galaxy.hpp` and
`galaxy_collision.hpp`, and what the model does and does not claim to be is
ADR-0038.

The run below is the one the figures in the next section come from. It is
deliberately smaller and shorter than the configuration file's defaults, so that
it finishes in two minutes and can be re-run by anyone reading this:

```
orrery run examples/collision.orrery \
    --set initial_conditions.count=20000 --set run.steps=6000 \
    --set output.trajectory_stride=20
orrery-view play collision.otj --exposure 1.6
```

It took 122 seconds, 20.4 ms per step, and conserved energy to 3.3 parts in a
thousand. Its virial ratio went from 0.94 at the start to 0.99 at the end, which
is the merger virialising: two galaxies each in rough internal balance, plus the
orbital energy of the encounter between them, become one object in balance with
itself.

The softening is the setting this scenario is most sensitive to, and the
configuration file explains the choice at length. The disc is cold and therefore
unstable to its own self-gravity, so it will fragment on every scale the force
law permits. At a softening of 0.12, a little above the disc's scale height, the
fragmentation is confined to scales the model does not resolve anyway and the
spiral structure and tidal tails are unaffected. At 0.05 the same run ends as a
swarm of small dense knots.

## What it costs

Measured on the machine [the performance report](performance.md) describes: Core
Ultra 5 238V with an integrated Arc 130V, Windows 11, Clang 22, the `renderer`
preset, double precision.

These figures come from the viewer's own frame counter rather than from the
Phase 7 benchmark harness. That is a weaker instrument and it is adequate here
for a specific reason: each figure below is already the mean of several hundred
consecutive frames, which is the repetition the harness exists to provide, and no
figure is a before-and-after comparison of the same kernel. A frame rate quoted
from a single frame would need the harness; a mean over four hundred does not.
They were taken with `--hidden --no-vsync`, because a visible window on Windows
is paced by the desktop compositor whatever the swap interval says, so a frame
rate measured from one is a measurement of the monitor.

**Drawing alone**, one static configuration redrawn, at 1280 by 720:

| Particles | Frames per second |
| --- | --- |
| 20 000 | 5020 |
| 60 000 | 2590 |
| 200 000 | 740 |
| 500 000 | 274 |
| 1 000 000 | 126 |

At 1920 by 1080 the same counts give 2100, 615 and 105 frames per second at
60 000, 200 000 and one million. The resolution costs little until the particle
count is small, which is what it should do: the per-particle work is the same and
only the tone mapping pass scales with the number of pixels.

**A live run**, one Barnes-Hut evaluation and one frame per turn, at 1280 by 720:

| Particles | Frames per second |
| --- | --- |
| 10 000 | 141 |
| 20 000 | 56 |
| 30 000 | 34 |
| 60 000 | 14 |

So the collision runs and draws at better than thirty frames a second up to about
thirty thousand particles, and the limit is the solver rather than the renderer:
at that count the renderer is drawing three thousand frames a second and waiting.
The headline is therefore two numbers rather than one. Interactively, thirty
thousand particles live. For a picture, a million particles at a hundred frames a
second, which is what playing a trajectory back reaches.

**Playing and exporting**, a 20 000-particle trajectory at 1280 by 720: 447
frames a second played to the screen, 61 frames a second exported to PPM. The
difference is the synchronous readback of the accumulated buffer and the file
write, and ADR-0036 records why the export reads back radiance and tone maps on
the host rather than reading back the finished pixels.

## Where the pieces are

| What | Where |
| --- | --- |
| Camera, projection, controls | `include/orrery/viz/camera.hpp` |
| The tone mapping curve, in C++ and GLSL | `include/orrery/viz/tone_map.hpp` |
| The renderer and its two passes | `include/orrery/viz/point_renderer.hpp` |
| The window, the context and the input | `include/orrery/viz/viewer_window.hpp` |
| The OpenGL entry points | `include/orrery/viz/gl_api.hpp` |
| The image and its PPM writer | `include/orrery/viz/image.hpp` |
| The viewer itself | `apps/orrery_view.cpp` |
