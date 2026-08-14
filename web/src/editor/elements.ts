/**
 * What a design is, worked out from what it says.
 *
 * Everything here is a transcription of a function in `src/initial_conditions/`
 * or `src/sim/assembly.cpp`, named after the one it transcribes, so that the
 * figures beside the drawing are the figures the sampler will use rather than a
 * second opinion about them. Where the C++ has a function, this calls it the
 * same thing and computes it the same way; where it does not, which is the
 * orbital elements of the encounter, the quantities follow from the placement by
 * the standard two-body relations and nothing else.
 *
 * The transcription is the risk this file carries, and it is why
 * `tests/editor/agreement.test.ts` runs a design through the compiled solver and
 * compares the state it starts in against what these functions say it should be.
 *
 * Units are the configuration's: G is one (ADR-0007), so a mass is a mass and a
 * speed is a length over a time and there is no constant to carry.
 */

import type { Design } from './design';

/** ADR-0007. Stated so that the formulae below read as they are written. */
const G = 1;

/**
 * `3 pi / 16`, the scale radius that puts a unit-mass Plummer sphere into
 * standard N-body units. `initial_conditions/plummer.hpp` gives the convention
 * and why it is worth following.
 */
export const STANDARD_PLUMMER_RADIUS = (3 * Math.PI) / 16;

/** A vector in the model's frame. Three numbers, and the two operations used. */
export interface Vec3 {
  readonly x: number;
  readonly y: number;
  readonly z: number;
}

export function norm(v: Vec3): number {
  return Math.hypot(v.x, v.y, v.z);
}

/**
 * A galaxy as `DiscGalaxyParameters` describes one.
 *
 * The masses are the disc's and the bulge's rather than a total and a fraction,
 * because that is the form the sampler takes and the form the enclosed mass is
 * computed from. `sim/assembly.cpp` does the conversion, and so does
 * `galaxyFrom` below.
 */
export interface Galaxy {
  readonly count: number;
  readonly discMass: number;
  readonly bulgeMass: number;
  readonly scaleLength: number;
  readonly scaleHeight: number;
  readonly bulgeRadius: number;
  readonly massFractionCutoff: number;
  readonly softening: number;
  readonly inclination: number;
  readonly positionAngle: number;
}

/**
 * The realised component masses, which are the requested ones rounded to whole
 * particles. `components_of` in `disc_galaxy.cpp`.
 */
export interface Components {
  readonly discCount: number;
  readonly bulgeCount: number;
  readonly particleMass: number;
  readonly discMass: number;
  readonly bulgeMass: number;
}

/** `disc_galaxy_disc_count`: the split by mass, rounded to the nearest particle. */
export function discCount(galaxy: Galaxy): number {
  if (galaxy.count === 0) return 0;

  const fraction = galaxy.discMass / (galaxy.discMass + galaxy.bulgeMass);
  const rounded = Math.round(fraction * galaxy.count);

  // At least one and no more than the count, as the C++ clamps it: a
  // configuration named for its disc should not be able to produce one with no
  // disc in it.
  if (rounded === 0) return 1;
  return rounded > galaxy.count ? galaxy.count : rounded;
}

export function components(galaxy: Galaxy): Components {
  if (galaxy.count === 0) {
    return { discCount: 0, bulgeCount: 0, particleMass: 0, discMass: 0, bulgeMass: 0 };
  }

  const disc = discCount(galaxy);
  const particleMass = (galaxy.discMass + galaxy.bulgeMass) / galaxy.count;
  return {
    discCount: disc,
    bulgeCount: galaxy.count - disc,
    particleMass,
    discMass: disc * particleMass,
    bulgeMass: (galaxy.count - disc) * particleMass,
  };
}

/**
 * `disc_galaxy_total_mass`: the mass the sample actually has.
 *
 * This rather than the requested mass is what an encounter has to be placed
 * with, since it is the mass that will actually attract.
 */
export function totalMass(galaxy: Galaxy): number {
  const parts = components(galaxy);
  return parts.discMass + parts.bulgeMass;
}

/** The fraction of an untruncated exponential disc's mass inside `x` scale lengths. */
function enclosedFraction(x: number): number {
  return 1 - (1 + x) * Math.exp(-x);
}

/**
 * The radius, in scale lengths, enclosing `fraction` of the untruncated disc.
 *
 * Bisection, as the C++ does it and for the reason it gives: the function has no
 * closed-form inverse, this runs at setup rather than in a step, and bisection
 * cannot fail to converge or leave the bracket.
 */
export function radiusEnclosing(fraction: number): number {
  let low = 0;
  let high = 1;
  while (enclosedFraction(high) < fraction) high *= 2;

  const tolerance = Number.EPSILON;
  while (high - low > tolerance * (1 + high)) {
    const middle = (low + high) / 2;
    if (enclosedFraction(middle) < fraction) low = middle;
    else high = middle;
  }
  return (low + high) / 2;
}

/** The mass of a Plummer sphere inside `radius`. */
export function plummerEnclosedMass(
  mass: number,
  scaleRadius: number,
  radius: number,
): number {
  const ratio = radius / Math.sqrt(radius * radius + scaleRadius * scaleRadius);
  return mass * ratio * ratio * ratio;
}

/** `disc_galaxy_enclosed_mass`: the mass inside cylindrical radius `radius`. */
export function enclosedMass(galaxy: Galaxy, radius: number): number {
  const parts = components(galaxy);
  if (radius <= 0) return 0;

  // Measured against the truncated sample rather than against the infinite
  // model, so that the enclosed mass reaches the whole of the disc at the
  // cutoff radius rather than one per cent short of it.
  const fraction =
    enclosedFraction(radius / galaxy.scaleLength) / galaxy.massFractionCutoff;
  const disc = parts.discMass * (fraction < 1 ? fraction : 1);

  return disc + plummerEnclosedMass(parts.bulgeMass, galaxy.bulgeRadius, radius);
}

/**
 * `disc_galaxy_circular_speed`: the speed of a circular orbit at `radius`.
 *
 * `v^2 = G M(R) R^2 / (R^2 + eps^2)^(3/2)`. The softening is in it because the
 * disc is placed on the orbits the run's own force law supports, which is the
 * whole of ADR-0038's last section.
 */
export function circularSpeed(galaxy: Galaxy, radius: number): number {
  if (radius <= 0) return 0;

  const softened = Math.sqrt(radius * radius + galaxy.softening * galaxy.softening);
  return Math.sqrt(
    (G * enclosedMass(galaxy, radius) * radius * radius) /
      (softened * softened * softened),
  );
}

/**
 * The z axis carried through the inclination and then the position angle.
 *
 * `rotate` in `disc_galaxy.cpp`, which is the one function both the spin axis
 * and the particles pass through, so that a galaxy's axis is by construction the
 * axis its particles were turned about.
 */
export function rotate(v: Vec3, inclination: number, positionAngle: number): Vec3 {
  const cosI = Math.cos(inclination);
  const sinI = Math.sin(inclination);
  const cosA = Math.cos(positionAngle);
  const sinA = Math.sin(positionAngle);

  const y = v.y * cosI - v.z * sinI;
  const z = v.y * sinI + v.z * cosI;

  return { x: v.x * cosA - y * sinA, y: v.x * sinA + y * cosA, z };
}

/** `disc_galaxy_spin_axis`: the unit vector the galaxy spins about. */
export function spinAxis(galaxy: Galaxy): Vec3 {
  return rotate({ x: 0, y: 0, z: 1 }, galaxy.inclination, galaxy.positionAngle);
}

/** The radius the disc's sample is truncated at. */
export function cutoffRadius(galaxy: Galaxy): number {
  return galaxy.scaleLength * radiusEnclosing(galaxy.massFractionCutoff);
}

/**
 * `galaxy_from` in `sim/assembly.cpp`: one galaxy of the size and mass asked
 * for, described by the settings everything else about a galaxy comes from.
 */
function galaxyFrom(design: Design, count: number, mass: number): Galaxy {
  return {
    count,
    discMass: mass * (1 - design.bulgeFraction),
    bulgeMass: mass * design.bulgeFraction,
    scaleLength: design.scaleLength,
    scaleHeight: design.scaleHeight,
    bulgeRadius: design.bulgeRadius,
    massFractionCutoff: design.massFractionCutoff,
    // The one setting that comes from another section: a disc is built at the
    // speeds the forces it will feel can support.
    softening: design.softening,
    inclination: design.inclination,
    positionAngle: design.positionAngle,
  };
}

/** The galaxy a `disc-galaxy` design describes, which takes all of the count. */
export function singleGalaxy(design: Design): Galaxy {
  return galaxyFrom(design, design.count, design.totalMass);
}

/** `primary_galaxy_count`: how the pair's particles are divided. */
export function primaryCount(design: Design): number {
  return Math.round(design.count / (1 + design.massRatio));
}

export function primaryGalaxy(design: Design): Galaxy {
  return galaxyFrom(
    design,
    primaryCount(design),
    design.totalMass / (1 + design.massRatio),
  );
}

/**
 * The second galaxy, which is smaller in size as well as in mass.
 *
 * Its three lengths are scaled by the square root of the mass ratio so that the
 * two have the same mean surface density, and its disc has its own orientation.
 * Both are `assembly.cpp`'s doing rather than the sampler's.
 */
export function secondaryGalaxy(design: Design): Galaxy {
  const size = Math.sqrt(design.massRatio);
  const galaxy = galaxyFrom(
    design,
    design.count - primaryCount(design),
    (design.totalMass * design.massRatio) / (1 + design.massRatio),
  );
  return {
    ...galaxy,
    scaleLength: galaxy.scaleLength * size,
    scaleHeight: galaxy.scaleHeight * size,
    bulgeRadius: galaxy.bulgeRadius * size,
    inclination: design.secondaryInclination,
    positionAngle: design.secondaryPositionAngle,
  };
}

/** `galaxy_collision_separation`: `(separation, impact_parameter, 0)`. */
export function separationVector(design: Design): Vec3 {
  return { x: design.separation, y: design.impactParameter, z: 0 };
}

/** The escape speed at the initial separation, which the approach speed multiplies. */
export function escapeSpeed(design: Design): number {
  const mass = totalMass(primaryGalaxy(design)) + totalMass(secondaryGalaxy(design));
  return Math.sqrt((2 * G * mass) / norm(separationVector(design)));
}

/**
 * `galaxy_collision_relative_velocity`: the secondary's velocity relative to the
 * primary.
 *
 * Along the x axis alone rather than along the separation, because an approach
 * directed along a separation tilted by the impact parameter would aim the
 * secondary at the primary's centre and produce a head-on collision started from
 * an oblique position.
 */
export function relativeVelocity(design: Design): Vec3 {
  return { x: -design.approachSpeed * escapeSpeed(design), y: 0, z: 0 };
}

/**
 * `galaxy_collision_orbit_energy`: `mu v^2 / 2 - G M1 M2 / d`.
 *
 * Negative for a bound pair, zero for the parabolic case and positive for a
 * hyperbolic one, which is the statement the approach speed makes.
 */
export function orbitEnergy(design: Design): number {
  const primary = totalMass(primaryGalaxy(design));
  const secondary = totalMass(secondaryGalaxy(design));
  const reduced = (primary * secondary) / (primary + secondary);
  const speed = norm(relativeVelocity(design));
  const distance = norm(separationVector(design));

  return (reduced * speed * speed) / 2 - (G * primary * secondary) / distance;
}

/**
 * How close a pair gets, whether they come back, and when they meet.
 *
 * The elements of the relative orbit, treating each galaxy as a point mass,
 * which is the approximation the placement itself makes and
 * `galaxy_collision.hpp` states: exact only while the separation is large
 * compared with the galaxies. What actually happens differs by the quadrupole of
 * each disc, so these are the design's intent rather than a prediction of the
 * run, and the interface says so.
 */
export interface Encounter {
  /** The masses that attract, which are the realised ones. */
  readonly primaryMass: number;
  readonly secondaryMass: number;
  readonly separation: number;
  readonly speed: number;
  readonly escapeSpeed: number;
  readonly energy: number;
  readonly bound: boolean;
  readonly eccentricity: number;
  /** The closest approach of the two centres. */
  readonly periapsis: number;
  /** Positive for a bound orbit, negative for an unbound one. */
  readonly semiMajorAxis: number;
  /** Model time from the start of the run to closest approach. */
  readonly timeToEncounter: number;
}

/**
 * The relative orbit of the two galaxies.
 *
 * Standard two-body relations from the position and velocity the placement
 * gives: the eccentricity from the energy and the angular momentum, the
 * periapsis from the semi-latus rectum, and the time to closest approach from
 * the anomaly that matches the case. There are three cases because the
 * approach speed reaches all three, and a formula written for the ellipse alone
 * would return nothing at all where the interface most needs an answer, which is
 * at the parabolic speed the encounter is defined against.
 */
export function encounterOf(design: Design): Encounter {
  const primaryMass = totalMass(primaryGalaxy(design));
  const secondaryMass = totalMass(secondaryGalaxy(design));
  const mu = G * (primaryMass + secondaryMass);

  const r = separationVector(design);
  const v = relativeVelocity(design);
  const distance = norm(r);
  const speed = norm(v);

  // The angular momentum of the relative orbit. The encounter is planar in x-y,
  // so this has only a z component, and it is the impact parameter times the
  // approach speed.
  const angularMomentum = Math.abs(r.x * v.y - r.y * v.x);

  const specificEnergy = (speed * speed) / 2 - mu / distance;
  const semiLatus = (angularMomentum * angularMomentum) / mu;
  const eccentricity = Math.sqrt(
    Math.max(0, 1 + (2 * specificEnergy * semiLatus) / mu),
  );
  const periapsis = semiLatus / (1 + eccentricity);

  return {
    primaryMass,
    secondaryMass,
    separation: distance,
    speed,
    escapeSpeed: escapeSpeed(design),
    energy: orbitEnergy(design),
    bound: specificEnergy < 0,
    eccentricity,
    periapsis,
    semiMajorAxis:
      specificEnergy === 0 ? Number.POSITIVE_INFINITY : -mu / (2 * specificEnergy),
    timeToEncounter: timeToPeriapsis(
      mu,
      distance,
      semiLatus,
      eccentricity,
      radial(r, v),
    ),
  };
}

/** Whether the two are closing, which decides the sign of the true anomaly. */
function radial(r: Vec3, v: Vec3): number {
  return r.x * v.x + r.y * v.y + r.z * v.z;
}

/** How near an eccentricity has to be to one to be treated as parabolic. */
const PARABOLIC = 1e-8;

/**
 * The time from the present state to periapsis.
 *
 * Negative if periapsis is behind rather than ahead, which is what a pair placed
 * moving apart would give and what the interface then says.
 */
function timeToPeriapsis(
  mu: number,
  distance: number,
  semiLatus: number,
  eccentricity: number,
  radialVelocity: number,
): number {
  if (!(eccentricity > 0)) {
    // A circular orbit has no periapsis: every point of it is the same distance
    // away, and there is no moment of closest approach to count towards.
    return Number.NaN;
  }

  // The true anomaly, taken negative while the two are still closing so that the
  // time below comes out positive.
  const cosine = Math.min(1, Math.max(-1, (semiLatus / distance - 1) / eccentricity));
  const anomaly = Math.sign(radialVelocity || 1) * Math.acos(cosine);
  const half = Math.tan(anomaly / 2);

  if (Math.abs(eccentricity - 1) < PARABOLIC) {
    // Barker's equation. The parabolic case is not a curiosity here: it is the
    // encounter an approach speed of exactly one asks for.
    const periapsis = semiLatus / 2;
    return (
      -Math.sqrt((2 * periapsis * periapsis * periapsis) / mu) * (half + half ** 3 / 3)
    );
  }

  if (eccentricity < 1) {
    const axis = semiLatus / (1 - eccentricity * eccentricity);
    const anomalyE =
      2 * Math.atan(Math.sqrt((1 - eccentricity) / (1 + eccentricity)) * half);
    const mean = anomalyE - eccentricity * Math.sin(anomalyE);
    return -mean / Math.sqrt(mu / (axis * axis * axis));
  }

  const axis = semiLatus / (eccentricity * eccentricity - 1);
  const anomalyH =
    2 * Math.atanh(Math.sqrt((eccentricity - 1) / (eccentricity + 1)) * half);
  const mean = eccentricity * Math.sinh(anomalyH) - anomalyH;
  return -mean / Math.sqrt(mu / (axis * axis * axis));
}

/** The elements of the two-body configuration, as `kepler.cpp` computes them. */
export interface KeplerElements {
  readonly period: number;
  readonly energy: number;
  readonly angularMomentum: number;
  readonly periapsis: number;
  /** The relative speed at periapsis, which is where the pair is released. */
  readonly speed: number;
}

export function keplerElements(design: Design): KeplerElements {
  const total = design.primaryMass + design.secondaryMass;
  const reduced = (design.primaryMass * design.secondaryMass) / total;
  const axis = design.semiMajorAxis;
  const periapsis = axis * (1 - design.eccentricity);

  return {
    period: 2 * Math.PI * Math.sqrt((axis * axis * axis) / (G * total)),
    energy: (-G * design.primaryMass * design.secondaryMass) / (2 * axis),
    angularMomentum:
      reduced *
      Math.sqrt(G * total * axis * (1 - design.eccentricity * design.eccentricity)),
    periapsis,
    // The vis-viva equation at periapsis, which is the one number that fixes the
    // orbit because the speed there is entirely transverse.
    speed: Math.sqrt((G * total * (1 + design.eccentricity)) / periapsis),
  };
}

/** The scale radius a Plummer design samples at, resolved as the C++ resolves it. */
export function resolvedScaleRadius(design: Design): number {
  return design.scaleRadius > 0 ? design.scaleRadius : STANDARD_PLUMMER_RADIUS;
}

/** The mass every particle of a sampled design carries. */
export function particleMass(design: Design): number {
  if (design.kind === 'kepler') return Number.NaN;
  if (design.kind === 'plummer') return design.totalMass / design.count;
  return components(
    design.kind === 'galaxy-collision' ? primaryGalaxy(design) : singleGalaxy(design),
  ).particleMass;
}
