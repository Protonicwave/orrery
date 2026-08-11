# ADR-0038: Sample the galaxy as a scenario rather than as an equilibrium model

- **Status:** Accepted
- **Date:** 2026-08-11

## Context

Every initial condition in the project before this one is either an exact
solution or an equilibrium: the Kepler orbit is analytic, the Plummer sphere is
a self-consistent solution of the collisionless Boltzmann equation, and the
uniform sphere is a deliberate non-equilibrium whose departure from one is stated
in closed form. That consistency is not an accident. It is what lets the
validation suite hold a run to a number rather than to an earlier run.

Phase 12 needs a galaxy, and a reviewer who has read the other four generators
will reasonably expect a fifth of the same kind.

## Decision

The disc galaxy is a scenario. It is a Plummer bulge inside an exponential disc
whose particles are placed on circular orbits, and it is not an equilibrium
solution in three separate ways, each named in
`include/orrery/initial_conditions/disc_galaxy.hpp`: the disc's own gravity is
treated as spherical when the circular speed is computed, the bulge is drawn as
though the disc were not there, and the disc is given no velocity dispersion at
all.

It is validated against what it claims rather than against equilibrium: the
particle count, equal masses, a centre of mass at rest at the origin, an angular
momentum along the spin axis, an enclosed mass function that matches the mass
actually sampled, and every disc particle on the circular orbit that function
supports.

## Alternatives considered

**A self-consistent disc.** The honest version of this model needs the
distribution function of a three-component system, which has no closed form, so
it is built either by iterating a distribution function to convergence or by
integrating the Jeans equations for the velocity moments and drawing from a
Gaussian approximation with an asymmetric drift correction. Both are real
methods, both are a phase of work on their own, and neither would change what
the demonstration shows. The literature's standard tools for this exist because
building such a model well is a research task.

**A warm disc, dispersion added without the correction.** Two lines of code, and
it would make the disc stable against its own self-gravity. It was rejected
because it is worse than either of the other two options: a disc given random
motions without the corresponding reduction in its mean rotation is out of
balance in a way that is neither stated nor small, so it expands over the first
dynamical time and the model would be wrong for a reason nothing in the code
admits to. The cold disc is out of equilibrium in a way that is stated and
understood.

**Use the Plummer sphere for the demonstration.** It is in equilibrium and it
would look like a fuzzy ball. Nearly all of a galaxy's kinetic energy is in
ordered rotation, and rotation is what produces the shape, the spiral structure
and the tidal tails a collision draws out. A demonstration of two colliding
Plummer spheres shows a slightly larger fuzzy ball.

## Consequences

The disc is Toomre unstable, with a stability parameter of zero everywhere, so
it develops spiral structure and a bar within a couple of rotations and, if the
force law lets it, breaks into clumps. That is the behaviour the demonstration
exists to show and it is also the model's largest artefact, so the demonstration
configuration chooses its softening to confine the fragmentation to scales the
model does not resolve anyway. `examples/collision.orrery` says so and gives the
value.

Nothing in the validation suite treats a galaxy as a conservation reference. The
Plummer sphere remains the equilibrium case and the uniform sphere the collapse
case, and both are unaffected by this.

A future phase that wants an equilibrium disc has somewhere to put it: the
parameters are already a structure, the sampler is one function, and a second
generator beside this one would not disturb anything. This ADR would then be
superseded rather than edited.

The model's softening is one of its parameters, which is unusual for an initial
condition and follows directly from the decision. A disc placed on circular
orbits has to be placed on the orbits the run's own force law supports, so the
assembly passes `solver.softening` through. That is the only setting in the
configuration format that crosses between sections.
