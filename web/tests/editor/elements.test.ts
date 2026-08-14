import { describe, expect, it } from 'vitest';
import { preset } from '../../src/editor/design';
import {
  circularSpeed,
  components,
  cutoffRadius,
  enclosedMass,
  encounterOf,
  escapeSpeed,
  keplerElements,
  norm,
  primaryCount,
  primaryGalaxy,
  radiusEnclosing,
  relativeVelocity,
  STANDARD_PLUMMER_RADIUS,
  secondaryGalaxy,
  separationVector,
  singleGalaxy,
  spinAxis,
  totalMass,
} from '../../src/editor/elements';

/**
 * The transcription of the C++ that samples a design.
 *
 * Every figure below is one the repository states somewhere else: in a header,
 * in an example configuration's comment, or in closed form. That is the point of
 * checking them here rather than against the code's own output, which would be
 * the code checking itself.
 */

describe('the two-body configuration', () => {
  const design = preset('kepler');

  it('has the period the example configuration quotes', () => {
    // examples/kepler.orrery: 2 pi sqrt(a^3 / G(m1 + m2)) is 4.443 for two unit
    // masses at a unit semi-major axis, and its timestep is that over two
    // hundred.
    expect(keplerElements(design).period).toBeCloseTo(4.443, 3);
    expect(keplerElements(design).period / design.timestep).toBeCloseTo(200, 6);
  });

  it('has the energy and the angular momentum the closed forms give', () => {
    const elements = keplerElements(design);
    expect(elements.energy).toBeCloseTo(-0.5, 12);
    expect(elements.angularMomentum).toBeCloseTo(0.5 * Math.sqrt(2 * 0.75), 12);
    expect(elements.periapsis).toBeCloseTo(0.5, 12);
    // Vis-viva at periapsis: sqrt(G M (1 + e) / r_p).
    expect(elements.speed).toBeCloseTo(Math.sqrt((2 * 1.5) / 0.5), 12);
  });
});

describe('the encounter', () => {
  const design = preset('galaxy-collision');

  it('is bound below the escape speed and parabolic at it', () => {
    expect(encounterOf(design).bound).toBe(true);
    expect(encounterOf(design).energy).toBeLessThan(0);

    // The parametrisation's whole claim: an approach speed of one is exactly
    // parabolic, so the orbit energy is exactly zero. `galaxy_collision.hpp`
    // says so and the C++ tests use it as an analytic check.
    const parabolic = encounterOf({ ...design, approachSpeed: 1 });
    expect(Math.abs(parabolic.energy)).toBeLessThan(1e-15);
    expect(parabolic.eccentricity).toBeCloseTo(1, 6);
    expect(parabolic.bound).toBe(false);

    const hyperbolic = encounterOf({ ...design, approachSpeed: 1.2 });
    expect(hyperbolic.energy).toBeGreaterThan(0);
    expect(hyperbolic.eccentricity).toBeGreaterThan(1);
  });

  it('approaches along the x axis at a multiple of the escape speed', () => {
    const speed = escapeSpeed(design);
    const mass = totalMass(primaryGalaxy(design)) + totalMass(secondaryGalaxy(design));
    expect(speed).toBeCloseTo(
      Math.sqrt((2 * mass) / norm(separationVector(design))),
      12,
    );

    const velocity = relativeVelocity(design);
    expect(velocity.x).toBeCloseTo(-design.approachSpeed * speed, 12);
    expect(velocity.y).toBe(0);
    expect(velocity.z).toBe(0);
  });

  it('meets at a periapsis inside the separation, and says when', () => {
    const encounter = encounterOf(design);
    expect(encounter.periapsis).toBeGreaterThan(0);
    expect(encounter.periapsis).toBeLessThan(encounter.separation);
    expect(encounter.timeToEncounter).toBeGreaterThan(0);

    // A pair that starts closer meets sooner, which is the one thing the
    // readout is for.
    const nearer = encounterOf({ ...design, separation: 10 });
    expect(nearer.timeToEncounter).toBeLessThan(encounter.timeToEncounter);
  });

  it('gives a head-on encounter no periapsis to speak of', () => {
    // No impact parameter is no angular momentum, so the two centres pass
    // through one another: the periapsis is zero and the pair is on a radial
    // orbit rather than an ellipse.
    const headOn = encounterOf({ ...design, impactParameter: 0 });
    expect(headOn.periapsis).toBe(0);
    expect(headOn.eccentricity).toBeCloseTo(1, 6);
  });

  it('divides the particles between the two galaxies as the assembly does', () => {
    // primary_galaxy_count: the count over one plus the mass ratio, rounded.
    expect(primaryCount({ ...design, count: 2000, massRatio: 0.5 })).toBe(1333);
    expect(primaryCount({ ...design, count: 60000, massRatio: 1 })).toBe(30000);

    const primary = primaryGalaxy(design);
    const secondary = secondaryGalaxy(design);
    expect(primary.count + secondary.count).toBe(design.count);

    // Both galaxies are made of particles of the same mass, which is what makes
    // a mass ratio a mass ratio rather than a resolution difference.
    expect(components(primary).particleMass).toBeCloseTo(
      components(secondary).particleMass,
      12,
    );

    // The smaller galaxy is smaller in size as well as in mass, by the square
    // root of the ratio, so the two have the same mean surface density.
    expect(secondary.scaleLength).toBeCloseTo(
      primary.scaleLength * Math.sqrt(design.massRatio),
      12,
    );
  });
});

describe('the disc galaxy', () => {
  const design = preset('disc-galaxy');
  const galaxy = singleGalaxy(design);

  it('is truncated where the header says it is', () => {
    // disc_galaxy.hpp: ninety-nine per cent of an exponential disc's mass lies
    // inside 6.64 scale lengths.
    expect(radiusEnclosing(0.99)).toBeCloseTo(6.638, 3);
    expect(cutoffRadius(galaxy)).toBeCloseTo(6.638 * galaxy.scaleLength, 3);
  });

  it('splits its particles by mass and keeps them all the same', () => {
    const parts = components(galaxy);
    expect(parts.discCount + parts.bulgeCount).toBe(design.count);
    expect(parts.discCount / design.count).toBeCloseTo(1 - design.bulgeFraction, 3);
    expect(parts.particleMass * design.count).toBeCloseTo(design.totalMass, 12);
    expect(totalMass(galaxy)).toBeCloseTo(design.totalMass, 12);
  });

  it('encloses the whole of its sampled mass at its edge', () => {
    const parts = components(galaxy);
    const edge = cutoffRadius(galaxy);
    expect(enclosedMass(galaxy, edge)).toBeGreaterThan(parts.discMass);
    expect(enclosedMass(galaxy, edge)).toBeLessThanOrEqual(totalMass(galaxy));
    expect(enclosedMass(galaxy, 0)).toBe(0);
  });

  it('turns at the speed the softened force law supports', () => {
    // v^2 = G M(R) R^2 / (R^2 + eps^2)^(3/2), which is the whole of the formula
    // and is stated in disc_galaxy.hpp.
    const radius = 1.5;
    const softened = Math.sqrt(radius * radius + design.softening * design.softening);
    const expected = Math.sqrt(
      (enclosedMass(galaxy, radius) * radius * radius) / softened ** 3,
    );
    expect(circularSpeed(galaxy, radius)).toBeCloseTo(expected, 12);

    // Softening lowers it, which is why the sampler is given the solver's value.
    const point = circularSpeed({ ...galaxy, softening: 0 }, radius);
    expect(circularSpeed(galaxy, radius)).toBeLessThan(point);
  });

  it('spins about the axis its inclination names', () => {
    const upright = spinAxis({ ...galaxy, inclination: 0, positionAngle: 0 });
    expect(upright.z).toBeCloseTo(1, 12);

    const tilted = spinAxis({ ...galaxy, inclination: Math.PI / 2, positionAngle: 0 });
    expect(tilted.z).toBeCloseTo(0, 12);
    expect(tilted.y).toBeCloseTo(-1, 12);

    // Turned all the way over is a retrograde disc, which is how one is asked
    // for: there is no separate flag.
    const over = spinAxis({ ...galaxy, inclination: Math.PI, positionAngle: 0 });
    expect(over.z).toBeCloseTo(-1, 12);
  });
});

describe('the Plummer sphere', () => {
  it('has the scale radius that puts it in standard units', () => {
    expect(STANDARD_PLUMMER_RADIUS).toBeCloseTo((3 * Math.PI) / 16, 15);
    expect(STANDARD_PLUMMER_RADIUS).toBeCloseTo(0.589, 3);
  });
});
