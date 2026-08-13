/**
 * The radial mass profile of a frame, worked out in the browser.
 *
 * This is what the console's derived tier is for. The rail's plots are what the
 * run measured and are read from the file it wrote (ADR-0048); this is a
 * quantity the run did not record and the client can produce, from the two
 * things a trajectory carries: the positions of one frame and the masses in its
 * header. Nothing about it is a second answer to a question the file already
 * answers.
 *
 * What it shows is how the mass of the system is arranged about its own centre
 * of mass, as a density against radius. It is the shape a model is named by: a
 * Plummer sphere is flat in the middle and falls as the inverse fifth power
 * outside its scale radius, and an encounter tears the outer part of that
 * profile off while leaving the middle of it alone.
 *
 * Logarithmic shells, because a density profile spans decades in both radius
 * and density and equal shells would put nine tenths of the particles in the
 * outermost one.
 */

/** One profile, in buffers that are written into rather than replaced. */
export interface Profile {
  /** Shells, from the innermost outwards. */
  readonly shells: number;
  /** The outer radius of each shell. */
  readonly radius: Float64Array;
  /** The mass in each shell divided by its volume. */
  readonly density: Float64Array;
  /** How many shells hold anything, counting from the innermost. */
  occupied: number;
  /** The largest density found, for scaling a plot against it. */
  peak: number;
}

/** Shells. Enough to see the shape of a profile, few enough to be quiet. */
const SHELLS = 28;

/**
 * The smallest radius the innermost shell covers, as a fraction of the
 * largest. Five decades, which is more than any of the published runs spans
 * and cheap to carry.
 */
const FLOOR = 1e-5;

export function createProfile(): Profile {
  return {
    shells: SHELLS,
    radius: new Float64Array(SHELLS),
    density: new Float64Array(SHELLS),
    occupied: 0,
    peak: 0,
  };
}

/**
 * Measure a frame into an existing profile.
 *
 * Allocates nothing: this is called ten times a second while a run plays, and
 * the two arrays it writes into were made once. The particle loop runs twice
 * over the whole frame, once for the centre and once for the shells, which at
 * eight thousand particles is sixteen thousand iterations of arithmetic and
 * about a tenth of a millisecond.
 */
export function measureProfile(
  x: Float32Array,
  y: Float32Array,
  z: Float32Array,
  masses: Float64Array,
  into: Profile,
): void {
  const count = Math.min(x.length, masses.length);
  const shells = into.density;
  into.density.fill(0);
  into.occupied = 0;
  into.peak = 0;
  if (count === 0) return;

  let total = 0;
  let centreX = 0;
  let centreY = 0;
  let centreZ = 0;
  for (let index = 0; index < count; index += 1) {
    const mass = masses[index] as number;
    total += mass;
    centreX += (x[index] as number) * mass;
    centreY += (y[index] as number) * mass;
    centreZ += (z[index] as number) * mass;
  }
  if (total <= 0) return;
  centreX /= total;
  centreY /= total;
  centreZ /= total;

  // The outermost shell is set by the particle furthest out, so the profile is
  // scaled to the frame rather than to a radius chosen in advance. An encounter
  // throws material a long way and the profile should follow it there.
  let furthest = 0;
  for (let index = 0; index < count; index += 1) {
    const dx = (x[index] as number) - centreX;
    const dy = (y[index] as number) - centreY;
    const dz = (z[index] as number) - centreZ;
    const squared = dx * dx + dy * dy + dz * dz;
    if (squared > furthest) furthest = squared;
  }
  furthest = Math.sqrt(furthest);
  if (!(furthest > 0)) return;

  const inner = furthest * FLOOR;
  const decades = Math.log(furthest / inner);
  for (let shell = 0; shell < SHELLS; shell += 1) {
    into.radius[shell] = inner * Math.exp((decades * (shell + 1)) / SHELLS);
  }

  for (let index = 0; index < count; index += 1) {
    const dx = (x[index] as number) - centreX;
    const dy = (y[index] as number) - centreY;
    const dz = (z[index] as number) - centreZ;
    const radius = Math.sqrt(dx * dx + dy * dy + dz * dz);

    // Which shell, by inverting the spacing rather than by searching it.
    // Everything inside the floor falls into the innermost shell, which is
    // where a particle exactly at the centre belongs.
    const shell =
      radius <= inner
        ? 0
        : Math.min(
            SHELLS - 1,
            Math.floor((Math.log(radius / inner) / decades) * SHELLS),
          );
    shells[shell] = (shells[shell] as number) + (masses[index] as number);
  }

  const third = 4.18879020478639; // 4 pi / 3
  for (let shell = 0; shell < SHELLS; shell += 1) {
    const outer = into.radius[shell] as number;
    const previous = shell === 0 ? 0 : (into.radius[shell - 1] as number);
    const volume = third * (outer * outer * outer - previous * previous * previous);
    const density = volume > 0 ? (shells[shell] as number) / volume : 0;
    shells[shell] = density;
    if (density > 0) {
      into.occupied = shell + 1;
      if (density > into.peak) into.peak = density;
    }
  }
}
