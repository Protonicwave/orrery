# ADR-0034: Draw with OpenGL 3.3 through GLFW

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Phase 12 asks for a real-time point renderer with additive blending and tone
mapping, an interactive camera, and offline frame export. Something has to put
pixels on a screen and something has to create the window and read the mouse.

The project already has a GPU backend, written in SYCL for the reasons ADR-0025
gives, and it already runs kernels on the same Arc 130V the renderer will draw
with. So the obvious first question is whether the renderer should be part of
that rather than a separate graphics path.

## Decision

OpenGL 3.3 core profile for drawing, GLFW for the window, the context and the
input. Both behind `ORRERY_ENABLE_RENDERER`, which is off by default.

## Alternatives considered

**Render through the SYCL backend.** SYCL is a compute API. It has no
rasteriser, no blending hardware, no swap chain and no path to a window, so
"rendering in SYCL" means writing a software rasteriser that runs on the GPU and
then finding some other way to display its output. The blending this renderer
needs is the fixed-function additive blending every raster pipeline has had for
thirty years, and reimplementing it as atomic adds into a buffer would be slower
and would be several hundred lines of code to do worse what the hardware does
for nothing. Interoperation between SYCL and a graphics API exists but is
vendor-specific and is exactly the sort of thing that turns a phase into two.

**Vulkan.** The modern choice, and the wrong one here by a wide margin. This
renderer draws one kind of primitive with one pipeline state into one
framebuffer. Vulkan's advantages are in controlling submission, synchronisation
and memory across many pipelines, and its cost is that the smallest program that
puts a triangle on the screen is over a thousand lines. There is nothing in this
renderer for that control to buy.

**A game engine or a scene graph.** Rejected on proportion and on dependency
weight. What is being drawn is a list of points; a scene graph exists to manage
a hierarchy of transforms and materials, and there is one transform and one
material.

**A plotting library, matplotlib through the Phase 13 bindings.** It would
produce figures for the validation report perfectly well and cannot produce this:
a million additively blended sprites at interactive rates is not what a plotting
library is for. Phase 13 will still want matplotlib, for the convergence and
scaling plots, and the two are not in competition.

**SDL or a platform's own window API instead of GLFW.** SDL is larger and brings
audio, joysticks and its own event loop; the platform APIs are three separate
implementations. GLFW does exactly the three things needed, in about twenty
thousand lines, with no dependencies of its own beyond the system's.

## Consequences

The renderer needs an OpenGL 3.3 driver, which every desktop GPU of the last
decade provides and no hosted continuous integration runner does. The CI job for
this configuration therefore compiles and runs the tests but cannot open a
window, and the layer is split so that most of it is testable without one: the
camera, the projection and the tone curve are arithmetic and are covered in every
build.

The option is off by default, as the SYCL backend is, so a machine with no
display still builds and tests the whole simulator.

Nothing above the renderer sees OpenGL or GLFW. `viz/viewer_window.hpp` exposes a
function pointer that resolves entry points, a size, a camera and a handful of
key queries, and no type from either library appears in any header. Replacing
either is then a change to one directory rather than to the viewer.

Drawing and computing share the same integrated GPU and the same memory
bandwidth. On this machine that is a real interaction rather than a theoretical
one, and it is why the measurements in `docs/visualisation.md` report the cost of
drawing and the cost of the solver separately as well as together.
