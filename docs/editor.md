# The initial-conditions editor

Design a run as a drawing, watch the same solver sample and step it, and take
away a configuration file this repository runs unmodified.

[The editor](https://protonicwave.github.io/orrery/instrument/editor/) is a page
of the browser client, beside [the instrument](instrument.md), and it is the
bridge back into the repository: what leaves it is an `.orrery` file, and
`orrery run` is the only thing needed to turn that file into the run it
described.

## What it edits

The parameters the configuration format actually has, under the names the format
gives them. There is no second vocabulary: a slider labelled "impact parameter"
sets `initial_conditions.impact_parameter`, and the file below it says so while
it is being dragged.

Four scenarios, arranged as a path rather than as a menu, because each is the
next one's starting point.

| Preset | What it is | Settings of its own |
| --- | --- | --- |
| Kepler two-body | The only system with a closed solution | Two masses, semi-major axis, eccentricity |
| Plummer sphere | The equilibrium the conservation tests use | Count, mass, scale radius |
| Single disc | That sphere given rotation and a shape | Bulge share, three lengths, inclination |
| Galaxy collision | Two discs on an encounter | Mass ratio, second tilt, separation, impact parameter, approach speed |

Moving along the path keeps what the next scenario can use and takes what it
cannot judge for itself. A count carried from a cluster into a collision is a
count somebody chose; a timestep carried there is a timestep chosen for a
cluster, and the two scenarios move at different speeds, so the run settings
follow the preset.

The secondary galaxy can also be dragged on the drawing, which sets the
separation and the impact parameter together. It is a way of typing two numbers
rather than a mode of its own: the same two have sliders, and the handle answers
to the arrow keys, so nothing on the page needs a pointer.

## What the drawing says

A plan on the x-y plane, which is the plane the encounter is planar in, so every
length in it is a true length and can be dimensioned. ADR-0053 records the
decision and what was rejected.

Each disc is drawn at its scale length and at the radius its sample is truncated
at, as the projection of its own circle, so an inclined disc is an ellipse
foreshortened by the cosine of its tilt rather than a smaller circle. Beside them
each galaxy has a section on its own line of nodes, where the inclination is an
angle in the plane of the paper and carries an arc and a reading in degrees; that
is the one view in which the number the file states can be dimensioned honestly.

The dashed curves are the orbit each galaxy is placed on, about the pair's centre
of mass. They are the two-body orbit the placement gives, treating each galaxy as
a point mass, which is exact only while the two are far apart: what happens after
the first passage is what the integration says happens, and the interface says
so where the elements are read.

## What it works out

The panel beside the drawing is derived from the settings before anything has
been run. For an encounter that is whether the pair is bound, the orbit energy,
the eccentricity, the periapsis and the model time to closest approach; for a
disc it is the component counts, the particle mass, the edge of the sample and
the circular speed the softened force law supports at the scale length; for the
two-body problem it is the period, the energy and the angular momentum in closed
form.

Those come from `web/src/editor/elements.ts`, which is a transcription of the C++
that samples a design: every function in it is named after the one it transcribes
in `src/initial_conditions/` or `src/sim/assembly.cpp`. That is a second copy of
formulae the repository already has, which it otherwise refuses to keep, and
ADR-0053 sets out why this one is worth its cost and what bounds it.

The approach speed is a multiple of the escape speed at the initial separation,
so one is exactly parabolic. That parametrisation is what lets the readout say
what kind of encounter a design is without any further calculation, and the unit
tests hold it to it: at an approach speed of one the orbit energy is zero to the
last bit.

## The preview

Asking for it starts the WebAssembly module in a Worker and gives it the design
as the text of a configuration file, exactly as the instrument's browser run
works ([`docs/webassembly.md`](webassembly.md) describes that build). It is cut
to three thousand particles and the first two thousand steps, and the panel
states the count that was sampled, the solver, the kernel, the thread count, the
measured step time, the energy drift and the virial ratio, all reported by the
module. A picture computed in a browser must not be readable as one of the
figures in the performance report.

It is drawn in the drawing's own projection and at the drawing's own scale, one
pixel per particle, on a surface under the plan. So the sample and the
construction lines register: the particles fill the ellipse the drawing ruled, or
they do not and one of the two is wrong.

The two galaxies are drawn apart, which the instrument cannot do for a published
run: a trajectory records positions and not which component a particle was
sampled into, but a design says how the count is divided and the sampler
documents that the primary's particles come first.

Editing while a preview is running starts it again once the editing has stopped
for four tenths of a second. A slider dragged across its travel produces a
hundred designs on the way, and sampling each of them would be a hundred Workers
started and killed to answer one movement.

## The file

The file is on screen while it is being made, and what is downloaded is what is
shown. It carries the comment header the configurations in `examples/` carry,
because that header is the most useful part of one of those files: what the
configuration is, what its elements come to, and the command that runs it. Every
figure in the prose is computed from the settings under it, so the two cannot
drift apart.

The client checks what the C++ configuration reader checks, in the reader's own
words, and a design it would refuse cannot be downloaded. Those checks are a
transcription of `problems_with` in `src/sim/configuration.cpp`.

## What says it works

A design out of the editor, run both ways, in
`web/tests/editor/agreement.test.ts`. The test writes the file, the fixture beside
it is one native run of that same file, and three things are compared.

The **file** the editor writes is byte for byte the one the native binary was
given, so a change to the writer fails the test rather than quietly producing a
different document.

The **placement** is measured from the native run's own first frame: the two
galaxies' centres of mass, the separation between them and the pair's barycentre,
against what `elements.ts` says they should be. That is the check on the
transcription, and it is made against the sampler's output rather than against
the code's own arithmetic.

The **integration** is compared between the native build and the WebAssembly one
over two hundred steps, as where each galaxy is at the end and as the order of
the energy error.

That last comparison is made on the two galaxies rather than particle by
particle, and the reason is worth stating. The two builds draw different bulges
from the same seed. `make_plummer_sphere` passes two draws from the random stream
as the two arguments of one call, and `make_disc_galaxy` draws a particle's height
and its sign as the two operands of a multiplication; C++ leaves the order of
evaluation unspecified in both cases, so two compilers may take them in opposite
orders. The result is a sample from the same distribution and not the same
sample. The disc's radii and angles are drawn by statements of their own and
agree exactly, which is why the test can still compare the disc particle for
particle in the one coordinate a height cannot reach.

That is a defect in the samplers rather than in the editor, and it is left alone
here: hoisting those calls into named locals would change every Plummer sample
this repository has published, including the gallery runs and the figures in the
reports. It is recorded here rather than fixed quietly.

## Where the pieces are

| What | Where |
| --- | --- |
| The design, and the file it writes | `web/src/editor/design.ts` |
| The comment header, computed from the design | `web/src/editor/description.ts` |
| The transcription of the samplers | `web/src/editor/elements.ts` |
| The drawing, as geometry | `web/src/editor/drawing.ts` |
| The surface it is drawn on | `web/src/editor/Sheet.tsx` |
| The preview's plan and its painter | `web/src/editor/preview.ts` |
| The console and the readout | `web/src/editor/Parameters.tsx`, `Readout.tsx` |
| The page | `web/editor/index.html`, `web/src/editor/Editor.tsx` |
