# ADR-0046: Put the renderer's device behind one interface

- **Status:** Accepted
- **Date:** 2026-08-13

## Context

The browser client has to draw a few hundred thousand point masses at sixty
frames a second, and there are two ways for a page to reach a graphics device.
WebGPU is the interface the platform is moving to and is the one that expresses
what this renderer wants: an explicit pipeline, a half-float render target and a
uniform buffer written once a frame. WebGL2 is the interface that runs
everywhere, including on the machines and browsers where WebGPU is either absent
or present without an adapter behind it.

Neither can be chosen alone. Shipping only WebGPU means an instrument that shows
a black rectangle to a substantial share of the people who open it, and the
share is not knowable in advance because it depends on the browser, the driver
and whether the machine has an adapter the browser is willing to expose. Shipping
only WebGL2 means writing against an interface that is being replaced, on a page
whose whole point is that it draws the same physics as the native renderer.

So there are two backends, and the question this record answers is where the
seam between them goes.

ADR-0026 answered the same question for the solver: the GPU sits behind the
solver interface rather than in front of it, so a caller asks for an
acceleration field and never learns which device evaluated it. The alternative
there, a caller that branched on the device, would have put the branch in every
caller and made the two paths free to drift.

## Decision

`web/src/render/renderer.ts` defines one interface: resize, draw, dispose, and
three facts about the backend that started. `webgpu.ts` and `webgl2.ts`
implement it and nothing else imports either of them. `create.ts` tries WebGPU,
falls back to WebGL2, and reports both reasons in one sentence if neither will
start.

The interface is deliberately narrow. Two implementations have to be kept
identical, and every method on the interface is a place they can differ, so the
interface holds the smallest set of operations that can draw a frame rather than
the set a graphics library would expose.

Both backends draw the same two passes as `src/viz/point_renderer.cpp`: every
particle as an instanced sprite with a Gaussian falloff, additively blended into
a half-float target with no depth test, then one full-screen pass applying the
extended Reinhard curve of `include/orrery/viz/tone_map.hpp`. The shaders are
transliterations of each other and of the GLSL in the native renderer.

Three things genuinely differ between the two, and each is stated where it
happens rather than hidden:

A sprite is an instanced quad in both, not a point. OpenGL has `gl_PointSize`
and WebGPU has no point size at all, so a port of the native renderer would have
had one backend drawing points and the other drawing quads. That is precisely
the difference that ends with the two producing different pictures, so neither
draws points.

The clip volume runs from minus one to one in z for WebGL2 and from zero to one
for WebGPU, so the camera's projection takes the convention as an argument and
each backend asks for its own. Nothing else changes: the two matrices differ in
one row and the picture is identical, which
`web/tests/render/camera.test.ts` checks.

The sRGB transfer function is applied in the shader in both, rather than by
asking the hardware for an sRGB target. WebGL2's default framebuffer is not an
sRGB one and has no equivalent of the native renderer's `GL_FRAMEBUFFER_SRGB`,
so one backend would have had the hardware do it and the other would have done
it by hand. Doing it by hand in both is what makes them write the same bytes,
and it is the same standard function `encode_srgb` applies for an exported frame.

## Alternatives considered

**WebGL2 only.** Half the code and none of this record. It would also mean that
the one part of the project written against a modern graphics interface is the
part written against the one being retired, and that the client cannot use the
compute path WebGPU offers when the WebAssembly solver arrives.

**WebGPU only, with a message for everyone else.** Honest, and it would have
made the renderer simpler in every dimension. It also makes the instrument
unavailable on machines that can plainly run it, which is a poor trade for a
page whose purpose is to be looked at.

**A third-party abstraction over both.** They exist and they are good. They are
also a dependency the size of the rest of the client, they abstract in the
direction of a general scene graph rather than in the direction of this
renderer's two passes, and the thing being abstracted here is about four hundred
lines per backend. ADR-0002's rule applies: a dependency needs a sentence saying
why it beats writing the code, and that sentence could not be written.

**Branching on the device at each call site.** The arrangement ADR-0026
declined for the solver, declined again here for the same reason.

## Consequences

The two backends have to be shown to agree, and a type cannot show it.
`web/e2e/plate.spec.ts` opens the published run in both, seeks both to the same
instant, and compares the two pictures as a sixteen by sixteen grid of mean
brightnesses. Not byte for byte: two drivers rasterising the same triangles do
not produce the same bytes. The grid is what catches the mistakes that actually
happen, an image flipped in y, a sprite size in the wrong units, a tone curve
applied twice. The backend is pinned with `?renderer=`, because a comparison
between the two is not a comparison if the machine chooses which one runs.

A machine with no WebGPU adapter, which is what continuous integration usually
is, skips that comparison rather than failing it. What matters there is that the
fallback works, and every other browser test exercises the fallback.

The WebGPU backend allocates on every frame by construction. The canvas hands
out a new texture each frame, so a view has to be made of it, and the work has
to be recorded into a command encoder that is then consumed. Those objects are
the API's rather than the renderer's; everything the renderer itself owns, the
pass descriptors included, is made once and mutated in place. Measured over a
minute of playback, the two backends allocate within two bytes a frame of each
other, so the difference does not show above what the browser costs anyway.

Adding a third backend later, or a compute path for the WebAssembly solver,
means implementing this interface and changing nothing that draws.
