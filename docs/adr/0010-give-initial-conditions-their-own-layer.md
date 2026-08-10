# ADR-0010: Give initial conditions their own layer

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Section 3 of the implementation plan lists six layers: `apps`, `sim`, `solvers`,
`integrators`, `backend` and `core`. `core` is described there as particle
storage, vector maths, diagnostics and memory.

Phase 3 adds two groups of code. The diagnostics are named in that description
and belong to `core` without argument. The initial conditions, a Plummer sphere
sampler, an exact Kepler two-body configuration and a uniform sphere, are named
nowhere. They depend on `core` and on nothing else, and no existing layer
describes them: they are not storage, not a solver, not an integrator, not a
backend, and not the simulation driver.

So they go into `core`, or `core` grows a description that no longer means
anything, or the plan gains a layer.

## Decision

Initial conditions are a layer of their own, in
`include/orrery/initial_conditions/` and `src/initial_conditions/`, in namespace
`orrery::initial_conditions`, built as `orrery::initial_conditions`. It depends
on `core` and nothing else depends on it except applications and tests. Section
3 of the implementation plan is updated to list it.

## Alternatives considered

**Put them in `core`.** No new layer, no change to the plan, and the dependency
direction is trivially satisfied. The cost is what `core` then means. Every
layer above includes `core` headers, so anything placed there is on the include
path of the entire project, and `core` is the layer whose contents get compiled
into every translation unit that touches a particle. A Plummer sampler is not a
foundational utility of the same kind as a 3-vector: it encodes a specific
astrophysical model, with a distribution function, a rejection constant and a
mass cutoff, none of which a force kernel should be able to reach. A layer
called `core` that contains the Plummer model is a layer whose name has stopped
describing it, and the next thing with nowhere else to go lands there too.

**Put them in `sim`.** The simulation driver does own the run's setup, so this
has some logic. It inverts the dependency for tests, which is where these are
used most: a solver test wanting a Plummer sphere would have to depend on the
layer above it, which the plan forbids and which would make `sim` a dependency
of nearly everything.

**Put them in the tests.** They are used mostly by tests today, so this is
honest about the present. It is wrong about the future: the command-line
application of Phase 11 and the Python bindings of Phase 13 both generate
initial conditions, and the visualisation of Phase 12 needs the galaxy collision
scenario. Code that ships cannot live in the test directory.

## Consequences

The plan's layer diagram changes, which is a change to the document that was
meant to be the fixed point. That is the honest cost, and the plan's own rule is
that a phase which turns out to differ from its description records the
difference here rather than pushing through quietly.

There is one more CMake target, one more test executable, and one more directory
for a reader to understand. In exchange, the physics models are separated from
the data structures, and a reviewer looking for the definition of the Plummer
sampling knows where it is from the directory name.

The layer will grow. The galaxy collision scenario of Phase 12 belongs here, and
so does whatever reads a configuration file into a set of particles once Phase
11 defines one. That is the argument for the boundary rather than against it: it
gives that growth somewhere to go that is not `core`.
