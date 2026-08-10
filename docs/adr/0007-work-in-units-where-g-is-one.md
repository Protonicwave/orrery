# ADR-0007: Work in units where the gravitational constant is one

- **Status:** Accepted
- **Date:** 2026-08-10

## Context

Every force evaluation and every energy diagnostic in this project multiplies by
the gravitational constant. Its value depends on the units the caller thinks in,
and the candidates are far apart: 6.674e-11 in SI, 4pi^2 when masses are in
solar masses, lengths in astronomical units and times in years, and 1 in the
N-body units the stellar dynamics literature uses.

The choice reaches further than the constant itself. It fixes the numerical
range of every intermediate product in the kernels, it decides whether a
published result can be compared against one of this project's without a
conversion, and it decides whether the constant is a compile-time value or a
member of a configuration object that every kernel has to be handed.

Two properties of this project make the question live rather than a matter of
taste. The single-precision build has about seven decimal digits and a limited
exponent range, and it is the configuration in which the largest runs happen.
And the validation results, the virial ratio of a Plummer sphere and the energy
of a Kepler orbit, are quoted in the literature in N-body units.

## Decision

`orrery::core::kGravitationalConstant` is a compile-time constant equal to one.
Masses, lengths and times are in N-body units throughout the library, and the
Plummer sampler's default scale radius of `3 pi / 16` is the one that makes a
unit-mass sphere's total energy exactly `-1/4`, which is the standard
normalisation of those units.

A caller whose system is in physical units scales it before it reaches the
library. That conversion belongs to the configuration layer that reads a run
description, which arrives in Phase 11.

## Alternatives considered

**Carry SI values.** The units a physicist writes down first, and the ones a
reader needs no explanation for. It puts a factor of 1e-11 into the innermost
multiplication of every kernel and makes the intermediate products span an
enormous range of exponents: a mass in kilograms is 1e30, a length in metres is
1e16, and their product overflows single precision. Every run would have to be
rescaled internally anyway, which is the decision above with an extra step and
an opportunity to get it wrong.

**Carry astronomical units.** Better behaved than SI and natural for planetary
work, with G equal to 4pi^2. It is the wrong convention for the systems this
project actually simulates, which are star clusters and colliding galaxies
rather than planetary systems, and it would put a conversion between every
result and the literature it is being checked against.

**Make the constant a run-time parameter.** The flexible answer: a
configuration object carrying G, threaded through the solvers to the kernels.
It costs a register in the inner loop, which is not the objection. The
objection is that it creates the possibility of a force kernel and an energy
diagnostic being given different values, which is exactly the class of error
that makes a conservation result meaningless, and the same argument that keeps
the softening in one place (ADR-0008) applies here.

## Consequences

Every input to the library is in N-body units, and nothing in the library
validates that, because nothing can: the numbers carry no units to check.
Documentation and the eventual configuration format are what stop a caller
passing kilograms.

The constant is one, so the compiler removes every multiplication by it. That
is a small saving in the kernels and a slightly awkward property in review,
since an expression that shows no `G` reads as though the physics were
forgotten. The constant is therefore written explicitly at each place it belongs
rather than left out, so the source shows a deliberate choice of units rather
than an omission.

Results are directly comparable with the cluster simulation literature, which is
the point. A virial ratio, a Plummer energy or a crossing time computed here can
be put beside a published one with no factor in between.

Changing this decision later means changing a constant and re-deriving the
tolerances of every test that compares against an analytic value, since those
tolerances are expressed relative to quantities of order one.
