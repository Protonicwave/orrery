# ADR-0053: Draw the initial conditions as a technical drawing

- **Status:** Accepted
- **Date:** 2026-08-14

## Context

The instrument plays runs the repository has already made. The obvious next
thing for it to do is let somebody make one, and the configuration format is
small enough that an editor for it is a reasonable size of work: a galaxy
collision is twelve numbers, and every one of them is a length, an angle, a mass
or a count.

What is not obvious is what the editor should show while those numbers are being
set. The scenario is three-dimensional, the client already has a renderer for
particles, and the natural first thought is to sample the design and draw it: a
small cloud of points that follows the sliders.

That would be a worse instrument than it looks. A cloud of a few thousand points
is a poor way to read a length: nothing in it is the scale length, the impact
parameter is not visible at all, and an inclination is a shape the viewer has to
infer. It also cannot show what has not happened yet, and most of what a person
setting up an encounter wants to know is exactly that, which is where the two
galaxies will pass and when.

## Decision

The editor draws the design as a technical drawing rather than as a picture of
it.

The main view is a plan on the x-y plane, which is the plane the encounter is
planar in, so the separation, the impact parameter and the orbit are true lengths
in it and can be dimensioned. Each disc is drawn as the projection of its own
circle, at its scale length and at the radius its sample is truncated at, with
its line of nodes. The predicted orbit of each galaxy about the pair's centre of
mass is a dashed construction line. The two settings that place the encounter
carry dimension lines with their measurements, the velocities are arrows at a
stated scale, and the angle between the separation and the x axis carries an arc
and a reading in degrees.

What a plan cannot show is the tilt of a disc, since that is a rotation out of
the plane the plan is drawn in. Each galaxy therefore also gets a section on its
own line of nodes, drawn to one side, where the disc is a line, the spin axis is
perpendicular to it and the angle between the disc and the x-y plane is the
inclination itself rather than a projection of it. That is the one view in which
the number the configuration states can be dimensioned honestly.

The WebAssembly preview is drawn underneath the plan, in the plan's own
projection and at the plan's own scale, by a second surface that plots one pixel
per particle. It is not the instrument's renderer.

The geometry is produced as data. `web/src/editor/drawing.ts` returns shapes with
positions in the units the configuration is written in, and a component puts them
on a surface. The figures those shapes carry come from
`web/src/editor/elements.ts`, which is a transcription of the C++ that will
sample the design, function by function and name by name.

## Alternatives considered

**Sample it and draw the particles.** The renderer exists, the solver is already
compiled to WebAssembly, and this is a few hours of work. It shows what the
configuration looks like and nothing about what it is. A drawing that cannot be
measured is a preview, and the thing being edited is a set of measurements.

**Draw the particles and put the numbers in a panel beside them.** Better, and
it is what the instrument does for a run it is playing. The trouble is that a
panel of numbers beside a picture is two documents: the number that says the
impact parameter is two and the picture that shows the encounter are related only
by the reader's belief that they are. A dimension line is the same statement made
once, in the place it applies to.

**A three-dimensional editor with handles in the scene.** This is what a modelling
tool would do, and it is the right answer when the thing being edited is a shape.
Here the thing being edited is a table of numbers with physical meanings, most of
which lie in one plane, and the two angles that do not are better served by a
section than by a camera the reader has to orbit until the tilt is legible.

**Reuse the plate's renderer for the preview.** It would have cost nothing to
write. It was rejected because the preview and the drawing have to register:
a perspective camera cannot be at the same scale as a plan, so the particles
would sit at one scale and the dimension lines at another, and the two halves of
the picture would contradict each other. The plate's optics are also wrong for
this. Additive blending into a half-float target and a tone-mapping pass exist to
show the sum of light along a line of sight, and what this needs to show is where
the particles are.

## Consequences

The drawing is testable without a browser, because the geometry is data.
`web/tests/editor/drawing.test.ts` asks whether a mark is where the sampler will
put a galaxy and whether an ellipse is foreshortened by the cosine of the
inclination, and neither question needs anything rendered.

There are now two independent statements about a design on one screen: the
drawing, worked out in the client from the settings, and the preview, produced by
the compiled C++ sampling the same settings. They are meant to agree, and the
value of the arrangement is that a disagreement is visible rather than
theoretical. `web/tests/editor/agreement.test.ts` makes the same comparison
against a native run.

The transcription in `elements.ts` is a second copy of formulae that already
exist in C++, which is a thing this repository otherwise refuses to have. It is
accepted here because the alternative is worse in both available directions:
calling into the WebAssembly module for every slider movement would make the
readout wait on a Worker to answer what the eccentricity is, and showing nothing
until a preview has been sampled would leave the editor unable to say what it is
about to make. The copy is bounded to one file, every function in it names the
one it transcribes, and the round-trip test measures the sampler's own output
against it.

A disc whose position angle is not zero is drawn correctly in the plan, but its
section is drawn on its line of nodes rather than on the x axis, so the two views
have different section planes. That is what makes the inclination readable, and
the section is labelled with which galaxy it belongs to rather than with the
plane it is taken on, which is a simplification a fuller drawing would not make.
