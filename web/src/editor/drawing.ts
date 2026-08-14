/**
 * The design as a technical drawing, in model coordinates.
 *
 * This file produces the drawing and draws none of it. It returns shapes with
 * their positions in the units the configuration is written in, and `Sheet.tsx`
 * puts them on a surface. Keeping the two apart is what makes the geometry
 * testable: whether the secondary galaxy is where the sampler will put it is a
 * question about numbers, and it should not have to be asked of a rendered
 * document.
 *
 * The view is a plan, looking down the z axis onto the x-y plane. That is the
 * plane the encounter is planar in (`galaxy_collision.hpp`), so the orbit, the
 * separation and the impact parameter are all true lengths in it and can be
 * dimensioned rather than foreshortened. What the plan cannot show is the tilt
 * of a disc, so each galaxy also gets a section on its own line of nodes, where
 * the inclination is an angle in the plane of the paper and can be dimensioned
 * exactly.
 */

import type { Design } from './design';
import {
  circularSpeed,
  components,
  cutoffRadius,
  encounterOf,
  type Galaxy,
  keplerElements,
  norm,
  primaryGalaxy,
  relativeVelocity,
  resolvedScaleRadius,
  rotate,
  secondaryGalaxy,
  separationVector,
  singleGalaxy,
  totalMass,
  type Vec3,
} from './elements';

export interface Point {
  readonly x: number;
  readonly y: number;
}

/**
 * What a line is for, which is what decides how it is drawn.
 *
 * A drawing has a hierarchy of line weights and it carries meaning: a body line
 * is the thing itself, a construction line is how it was arrived at, furniture
 * is the paper it sits on, and the accent is the one thing being pointed at.
 */
export type Weight = 'body' | 'construction' | 'furniture' | 'accent';

export type Shape =
  | {
      readonly kind: 'path';
      readonly points: readonly Point[];
      readonly weight: Weight;
      readonly dashed?: boolean;
      readonly closed?: boolean;
    }
  | {
      /** A circle of the model carried through a rotation, so an ellipse here. */
      readonly kind: 'ellipse';
      readonly centre: Point;
      readonly u: Point;
      readonly v: Point;
      readonly weight: Weight;
      readonly dashed?: boolean;
    }
  | { readonly kind: 'mark'; readonly at: Point; readonly label?: string }
  | {
      readonly kind: 'arrow';
      readonly from: Point;
      readonly to: Point;
      readonly weight: Weight;
      readonly label?: string;
    }
  | {
      /** A dimension line with its measurement, offset from what it measures. */
      readonly kind: 'dimension';
      readonly from: Point;
      readonly to: Point;
      readonly label: string;
      /** Pixels perpendicular to the run of the line, signed. */
      readonly offset: number;
    }
  | {
      readonly kind: 'angle';
      readonly at: Point;
      /** Pixels, since an angle has no length in the model. */
      readonly radius: number;
      readonly from: number;
      readonly to: number;
      readonly label: string;
    }
  | {
      readonly kind: 'text';
      readonly at: Point;
      readonly text: string;
      readonly anchor: 'start' | 'middle' | 'end';
      /** Pixels, so a label can sit clear of what it names at any scale. */
      readonly dx?: number;
      readonly dy?: number;
      readonly weight?: Weight;
    };

/**
 * A section through a disc on its own line of nodes.
 *
 * Drawn to one side of the plan rather than in it. In this view the disc is a
 * line, the spin axis is perpendicular to it, and the angle between the disc and
 * the x-y plane is the inclination itself rather than a projection of it, so the
 * arc can carry the number the configuration states.
 */
export interface Section {
  readonly title: string;
  readonly inclination: number;
  /** The direction the disc turns in, which the inclination decides. */
  readonly retrograde: boolean;
}

export interface Drawing {
  readonly shapes: readonly Shape[];
  readonly sections: readonly Section[];
  /** The half-width of the model the view has to cover, in model units. */
  readonly extent: number;
  /** Model lengths a unit of speed is drawn as, or null where nothing moves. */
  readonly velocityScale: number | null;
  /** What the drawing is of, for the reader who cannot see it. */
  readonly description: string;
}

/** Degrees, set as a drawing sets them. */
function degrees(radians: number): string {
  return `${((radians * 180) / Math.PI).toFixed(1)}°`;
}

/** A measurement, to the precision a length of this size is worth stating at. */
function measure(value: number): string {
  const magnitude = Math.abs(value);
  const digits = magnitude >= 100 ? 0 : magnitude >= 10 ? 1 : magnitude >= 1 ? 2 : 3;
  return value.toFixed(digits).replace('-', '−');
}

function plan(v: Vec3): Point {
  return { x: v.x, y: v.y };
}

function add(a: Point, b: Point): Point {
  return { x: a.x + b.x, y: a.y + b.y };
}

function scale(a: Point, k: number): Point {
  return { x: a.x * k, y: a.y * k };
}

/**
 * The two vectors a circle in the disc's plane projects onto.
 *
 * A circle of radius R in the disc is `R (u cos t + v sin t)` for the disc's own
 * axes u and v, and both of those are the model axes carried through the same
 * rotation the particles are. So the projected shape is an ellipse with those
 * two vectors as its conjugate semi-diameters, which is exactly what an ellipse
 * drawn through a transform takes.
 */
function discAxes(galaxy: Galaxy, radius: number): { u: Point; v: Point } {
  const u = rotate({ x: 1, y: 0, z: 0 }, galaxy.inclination, galaxy.positionAngle);
  const v = rotate({ x: 0, y: 1, z: 0 }, galaxy.inclination, galaxy.positionAngle);
  return { u: scale(plan(u), radius), v: scale(plan(v), radius) };
}

/** The line of nodes: where the disc's plane meets the x-y plane. */
function lineOfNodes(galaxy: Galaxy, centre: Point, radius: number): Shape {
  const u = plan(
    rotate({ x: 1, y: 0, z: 0 }, galaxy.inclination, galaxy.positionAngle),
  );
  return {
    kind: 'path',
    points: [add(centre, scale(u, -radius)), add(centre, scale(u, radius))],
    weight: 'construction',
    dashed: true,
  };
}

/**
 * One galaxy: the disc at its edge, at its scale length, and its bulge.
 *
 * Three ellipses rather than a filled shape, because a drawing states the
 * dimensions a thing was made to rather than what it looks like. The edge is the
 * radius the sample is truncated at, the middle one is the scale length the
 * exponential is written in, and the small one is the bulge's scale radius.
 */
function galaxyShapes(galaxy: Galaxy, centre: Point, label: string): Shape[] {
  const edge = cutoffRadius(galaxy);
  const parts = components(galaxy);

  const shapes: Shape[] = [
    {
      kind: 'ellipse',
      centre,
      ...discAxes(galaxy, edge),
      weight: 'construction',
      dashed: true,
    },
    {
      kind: 'ellipse',
      centre,
      ...discAxes(galaxy, galaxy.scaleLength),
      weight: 'body',
    },
    { kind: 'mark', at: centre },
    lineOfNodes(galaxy, centre, edge),
  ];

  if (parts.bulgeMass > 0) {
    shapes.push({
      kind: 'ellipse',
      centre,
      u: { x: galaxy.bulgeRadius, y: 0 },
      v: { x: 0, y: galaxy.bulgeRadius },
      weight: 'body',
    });
  }

  // Above the disc rather than on it, because a name written across a drawing
  // is a name that has to be read through whatever it is naming.
  shapes.push({
    kind: 'text',
    at: { x: centre.x, y: centre.y + edge },
    text: label,
    anchor: 'middle',
    dy: -8,
  });

  return shapes;
}

/**
 * The conic two bodies follow about their common focus.
 *
 * Sampled in true anomaly from the eccentricity vector, so it works for the
 * ellipse, the parabola and the hyperbola alike. An unbound orbit is drawn only
 * as far as a few times the initial separation, since the rest of it is a pair
 * of lines going nowhere.
 */
function conic(mu: number, r: Vec3, v: Vec3, reach: number): readonly Point[] {
  const distance = norm(r);
  const speed = norm(v);
  const radial = (r.x * v.x + r.y * v.y) / distance;

  // The eccentricity vector points at periapsis and its length is the
  // eccentricity, which is the whole of the orbit's shape and orientation.
  const factor = (speed * speed - mu / distance) / mu;
  const ex = factor * r.x - ((distance * radial) / mu) * v.x;
  const ey = factor * r.y - ((distance * radial) / mu) * v.y;
  const eccentricity = Math.hypot(ex, ey);
  const argument = Math.atan2(ey, ex);

  const angularMomentum = r.x * v.y - r.y * v.x;
  const semiLatus = (angularMomentum * angularMomentum) / mu;

  const points: Point[] = [];
  const steps = 240;
  for (let index = 0; index <= steps; index += 1) {
    const anomaly = -Math.PI + (2 * Math.PI * index) / steps;
    const denominator = 1 + eccentricity * Math.cos(anomaly);
    // The asymptotes of an unbound orbit, where the radius runs away.
    if (denominator <= 1e-6) continue;
    const radius = semiLatus / denominator;
    if (radius > reach) continue;
    const angle = argument + anomaly;
    points.push({ x: radius * Math.cos(angle), y: radius * Math.sin(angle) });
  }
  return points;
}

/** The velocity arrows, at a scale stated in the legend rather than assumed. */
function velocityArrows(
  places: readonly { at: Point; velocity: Point }[],
  extent: number,
): { shapes: Shape[]; scale: number | null } {
  const fastest = Math.max(
    ...places.map((place) => Math.hypot(place.velocity.x, place.velocity.y)),
  );
  if (!(fastest > 0)) return { shapes: [], scale: null };

  // The longest arrow is a fifth of the view. A velocity has no length, so the
  // scale is a choice, and a choice has to be stated: the legend gives it.
  const factor = (extent * 0.2) / fastest;
  const shapes = places.map((place): Shape => {
    const speed = Math.hypot(place.velocity.x, place.velocity.y);
    return {
      kind: 'arrow',
      from: place.at,
      to: add(place.at, scale(place.velocity, factor)),
      weight: 'accent',
      label: measure(speed),
    };
  });
  return { shapes, scale: factor };
}

function collisionDrawing(design: Design): Drawing {
  const primary = primaryGalaxy(design);
  const secondary = secondaryGalaxy(design);
  const primaryMass = totalMass(primary);
  const secondaryMass = totalMass(secondary);
  const total = primaryMass + secondaryMass;

  const separation = plan(separationVector(design));
  const relative = plan(relativeVelocity(design));

  // The pair is placed in its own centre-of-mass frame, so each galaxy sits on
  // its own side of the origin in inverse proportion to its mass, and the same
  // for the velocities. `make_galaxy_collision` splits both this way.
  const first = scale(separation, -secondaryMass / total);
  const second = scale(separation, primaryMass / total);
  const firstVelocity = scale(relative, -secondaryMass / total);
  const secondVelocity = scale(relative, primaryMass / total);

  const encounter = encounterOf(design);
  const extent = Math.max(
    norm(separationVector(design)) * 0.75,
    cutoffRadius(primary) * 2.2,
    1,
  );

  const shapes: Shape[] = [];

  // The orbit each galaxy is placed on, about the pair's centre of mass. The
  // relative conic scaled by the other galaxy's share of the mass is the path
  // this one follows, which is why there are two curves and not one. It is cut
  // off at the edge of the sheet, as a drawing is.
  const reach = extent * 1.4;
  const relativePath = conic(
    total,
    separationVector(design),
    relativeVelocity(design),
    reach,
  );
  if (relativePath.length > 1) {
    shapes.push(
      {
        kind: 'path',
        points: relativePath.map((point) => scale(point, -secondaryMass / total)),
        weight: 'construction',
        dashed: true,
      },
      {
        kind: 'path',
        points: relativePath.map((point) => scale(point, primaryMass / total)),
        weight: 'construction',
        dashed: true,
      },
    );
  }

  shapes.push(
    { kind: 'mark', at: { x: 0, y: 0 }, label: 'centre of mass' },
    {
      kind: 'path',
      points: [first, second],
      weight: 'construction',
      dashed: true,
    },
    {
      kind: 'angle',
      at: first,
      radius: 52,
      from: 0,
      to: Math.atan2(separation.y, separation.x),
      label: degrees(Math.atan2(separation.y, separation.x)),
    },
  );

  shapes.push(
    ...galaxyShapes(primary, first, `primary · ${measure(primaryMass)}`),
    ...galaxyShapes(secondary, second, `secondary · ${measure(secondaryMass)}`),
  );

  // The two settings the encounter is described by, dimensioned as lengths
  // rather than stated as numbers in a panel, because they are lengths.
  shapes.push(
    {
      kind: 'dimension',
      from: { x: first.x, y: first.y },
      to: { x: second.x, y: first.y },
      label: `separation ${measure(design.separation)}`,
      offset: 46,
    },
    {
      kind: 'dimension',
      from: { x: second.x, y: first.y },
      to: second,
      label: `b ${measure(design.impactParameter)}`,
      offset: 26,
    },
  );

  const arrows = velocityArrows(
    [
      { at: first, velocity: firstVelocity },
      { at: second, velocity: secondVelocity },
    ],
    extent,
  );
  shapes.push(...arrows.shapes);

  return {
    shapes,
    sections: [
      {
        title: 'primary',
        inclination: design.inclination,
        retrograde: design.inclination > Math.PI / 2,
      },
      {
        title: 'secondary',
        inclination: design.secondaryInclination,
        retrograde: design.secondaryInclination > Math.PI / 2,
      },
    ],
    extent,
    velocityScale: arrows.scale,
    description:
      `Two disc galaxies, ${measure(encounter.separation)} apart, on ` +
      `${encounter.bound ? 'a bound' : 'an unbound'} orbit with a periapsis of ` +
      `${measure(encounter.periapsis)}, drawn in plan with their orbits, ` +
      'their velocities and the separation dimensioned.',
  };
}

function discDrawing(design: Design): Drawing {
  const galaxy = singleGalaxy(design);
  const centre: Point = { x: 0, y: 0 };
  const edge = cutoffRadius(galaxy);
  const extent = edge * 1.35;

  const shapes: Shape[] = [
    ...galaxyShapes(galaxy, centre, `${measure(totalMass(galaxy))} in total`),
    {
      kind: 'dimension',
      from: centre,
      to: { x: galaxy.scaleLength, y: 0 },
      label: `R_d ${measure(galaxy.scaleLength)}`,
      offset: -22,
    },
    {
      kind: 'dimension',
      from: centre,
      to: { x: 0, y: edge },
      label: `edge ${measure(edge)}`,
      offset: 22,
    },
  ];

  // Which way the disc turns, drawn where it can be measured: a particle one
  // scale length out along the line of nodes, moving at the circular speed that
  // radius supports. An inclination past a right angle turns this arrow round,
  // which is how a retrograde disc is asked for and how it should read.
  const along = plan(
    rotate({ x: 1, y: 0, z: 0 }, galaxy.inclination, galaxy.positionAngle),
  );
  const across = plan(
    rotate({ x: 0, y: 1, z: 0 }, galaxy.inclination, galaxy.positionAngle),
  );
  const at = add(centre, scale(along, galaxy.scaleLength));
  shapes.push({
    kind: 'arrow',
    from: at,
    to: add(at, scale(across, extent * 0.18)),
    weight: 'accent',
    label: measure(circularSpeed(galaxy, galaxy.scaleLength)),
  });

  return {
    shapes,
    sections: [
      {
        title: 'disc',
        inclination: galaxy.inclination,
        retrograde: galaxy.inclination > Math.PI / 2,
      },
    ],
    extent,
    velocityScale: null,
    description:
      `One disc galaxy of ${measure(totalMass(galaxy))} in ${design.count} particles, ` +
      `drawn in plan at its scale length and its edge, inclined by ${degrees(galaxy.inclination)}.`,
  };
}

function plummerDrawing(design: Design): Drawing {
  const radius = resolvedScaleRadius(design);

  // The radius enclosing the sampled fraction, by inverting the Plummer mass
  // profile, which unlike the disc's has a closed-form inverse.
  const share = design.massFractionCutoff ** (2 / 3);
  const edge = radius * Math.sqrt(share / (1 - share));
  const extent = edge * 1.2;
  const centre: Point = { x: 0, y: 0 };

  const circle = (r: number, weight: Weight, dashed = false): Shape => ({
    kind: 'ellipse',
    centre,
    u: { x: r, y: 0 },
    v: { x: 0, y: r },
    weight,
    dashed,
  });

  return {
    shapes: [
      circle(edge, 'construction', true),
      circle(radius, 'body'),
      { kind: 'mark', at: centre },
      {
        kind: 'dimension',
        from: centre,
        to: { x: radius, y: 0 },
        label: `a ${measure(radius)}`,
        offset: -22,
      },
      {
        kind: 'dimension',
        from: centre,
        to: { x: 0, y: edge },
        label: `${measure(design.massFractionCutoff * 100)}% inside ${measure(edge)}`,
        offset: 22,
      },
      {
        kind: 'text',
        at: { x: 0, y: -edge },
        text: 'isotropic velocities, no net rotation',
        anchor: 'middle',
        dy: 16,
        weight: 'furniture',
      },
    ],
    sections: [],
    extent,
    velocityScale: null,
    description:
      `A Plummer sphere of ${design.count} particles at scale radius ${measure(radius)}, ` +
      `drawn as its scale radius and the sphere holding ${measure(design.massFractionCutoff * 100)} per cent of the mass.`,
  };
}

function keplerDrawing(design: Design): Drawing {
  const elements = keplerElements(design);
  const total = design.primaryMass + design.secondaryMass;
  const firstShare = design.secondaryMass / total;
  const secondShare = design.primaryMass / total;

  // The placement `make_kepler_orbit` gives: released at periapsis on the x
  // axis, moving in opposite directions along y.
  const separation = elements.periapsis;
  const first: Point = { x: -firstShare * separation, y: 0 };
  const second: Point = { x: secondShare * separation, y: 0 };
  const apoapsis = design.semiMajorAxis * (1 + design.eccentricity);
  const extent = apoapsis * 1.15;

  const relative = conic(
    total,
    { x: separation, y: 0, z: 0 },
    { x: 0, y: elements.speed, z: 0 },
    extent * 4,
  );

  const arrows = velocityArrows(
    [
      { at: first, velocity: { x: 0, y: -firstShare * elements.speed } },
      { at: second, velocity: { x: 0, y: secondShare * elements.speed } },
    ],
    extent,
  );

  return {
    shapes: [
      {
        kind: 'path',
        points: relative.map((point) => scale(point, -firstShare)),
        weight: 'construction',
        dashed: true,
        closed: true,
      },
      {
        kind: 'path',
        points: relative.map((point) => scale(point, secondShare)),
        weight: 'construction',
        dashed: true,
        closed: true,
      },
      { kind: 'mark', at: { x: 0, y: 0 } },
      { kind: 'mark', at: first },
      { kind: 'mark', at: second },
      {
        kind: 'text',
        at: { x: 0, y: 0 },
        text: 'centre of mass',
        anchor: 'middle',
        dy: 18,
      },
      {
        kind: 'text',
        at: first,
        text: `m₁ ${measure(design.primaryMass)}`,
        anchor: 'end',
        dx: -8,
        dy: -6,
      },
      {
        kind: 'text',
        at: second,
        text: `m₂ ${measure(design.secondaryMass)}`,
        anchor: 'start',
        dx: 8,
        dy: -6,
      },
      {
        kind: 'dimension',
        from: first,
        to: second,
        label: `periapsis ${measure(separation)}`,
        offset: 30,
      },
      {
        // The secondary's own ellipse rather than the relative orbit, because
        // that is the curve drawn beside it. The relative orbit's semi-major
        // axis is the setting, and it is in the readout.
        kind: 'dimension',
        from: { x: -apoapsis * secondShare, y: 0 },
        to: { x: separation * secondShare, y: 0 },
        label: `2a₂ ${measure((apoapsis + separation) * secondShare)}`,
        offset: -34,
      },
      ...arrows.shapes,
    ],
    sections: [],
    extent,
    velocityScale: arrows.scale,
    description:
      `Two point masses on an orbit of eccentricity ${measure(design.eccentricity)}, ` +
      `released at a periapsis separation of ${measure(separation)}, drawn with both ` +
      'orbits about their centre of mass.',
  };
}

/** The drawing of a design, whichever kind it is. */
export function drawingOf(design: Design): Drawing {
  switch (design.kind) {
    case 'kepler':
      return keplerDrawing(design);
    case 'plummer':
      return plummerDrawing(design);
    case 'disc-galaxy':
      return discDrawing(design);
    case 'galaxy-collision':
      return collisionDrawing(design);
  }
}
