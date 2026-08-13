import { describe, expect, it } from 'vitest';
import { createProfile, measureProfile } from '../../src/diagnostics/profile';

function measure(
  points: readonly (readonly [number, number, number])[],
  masses: readonly number[],
) {
  const profile = createProfile();
  measureProfile(
    Float32Array.from(points.map((point) => point[0])),
    Float32Array.from(points.map((point) => point[1])),
    Float32Array.from(points.map((point) => point[2])),
    Float64Array.from(masses),
    profile,
  );
  return profile;
}

describe('measureProfile', () => {
  /**
   * The centre is the centre of mass and not the mean position. Masses of one
   * and three at zero and four put it at three, and the two particles are then
   * at radius three and one: the heavier one is nearer the centre it dominates.
   */
  it('measures about the centre of mass', () => {
    const profile = measure(
      [
        [0, 0, 0],
        [4, 0, 0],
      ],
      [1, 3],
    );

    // The outermost shell is the furthest particle, which is the light one.
    expect(profile.radius[profile.shells - 1]).toBeCloseTo(3, 9);
  });

  /** All the mass is somewhere, so all of it is in some shell. */
  it('puts every particle in a shell', () => {
    const points: [number, number, number][] = [];
    const masses: number[] = [];
    for (let index = 0; index < 64; index += 1) {
      const radius = 0.1 + index * 0.05;
      points.push([radius, 0, 0]);
      masses.push(1);
    }
    const profile = measure(points, masses);

    // Mass is recovered by multiplying each shell's density back by its volume.
    let total = 0;
    const third = (4 * Math.PI) / 3;
    for (let shell = 0; shell < profile.shells; shell += 1) {
      const outer = profile.radius[shell] as number;
      const inner = shell === 0 ? 0 : (profile.radius[shell - 1] as number);
      total += (profile.density[shell] as number) * third * (outer ** 3 - inner ** 3);
    }
    expect(total).toBeCloseTo(64, 6);
  });

  /**
   * A uniform sphere has a flat density profile, which is the shape this is
   * meant to be able to show. Sampled as a cube root of a uniform draw, because
   * a radius drawn uniformly would pile the particles into the middle, which is
   * the mistake `initial_conditions/uniform_sphere` is tested against.
   */
  it('finds a uniform sphere to be uniform', () => {
    const points: [number, number, number][] = [];
    const masses: number[] = [];
    let seed = 20260813;
    const next = () => {
      seed = (seed * 1103515245 + 12345) >>> 0;
      return seed / 4294967296;
    };

    for (let index = 0; index < 20000; index += 1) {
      const radius = Math.cbrt(next());
      const cosine = 2 * next() - 1;
      const azimuth = 2 * Math.PI * next();
      const sine = Math.sqrt(1 - cosine * cosine);
      points.push([
        radius * sine * Math.cos(azimuth),
        radius * sine * Math.sin(azimuth),
        radius * cosine,
      ]);
      masses.push(1);
    }

    const profile = measure(points, masses);

    // The shells that hold enough particles to have a density worth reading.
    // The innermost few of twenty-eight logarithmic shells inside a unit sphere
    // hold a millionth of the volume between them and are empty at this count.
    const populated: number[] = [];
    for (let shell = 0; shell < profile.shells; shell += 1) {
      const density = profile.density[shell] as number;
      if (density > 0 && (profile.radius[shell] as number) > 0.2) {
        populated.push(density);
      }
    }

    expect(populated.length).toBeGreaterThanOrEqual(4);
    const mean = populated.reduce((a, b) => a + b, 0) / populated.length;
    for (const density of populated) {
      expect(Math.abs(density - mean) / mean).toBeLessThan(0.2);
    }
  });

  it('measures nothing from nothing', () => {
    const profile = measure([], []);
    expect(profile.occupied).toBe(0);
    expect(profile.peak).toBe(0);
  });

  /** One particle is at its own centre, so there is no radius to bin it by. */
  it('measures nothing from a configuration with no size', () => {
    const profile = measure([[3, 3, 3]], [1]);
    expect(profile.occupied).toBe(0);
  });

  it('writes into the profile it was given rather than making one', () => {
    const profile = createProfile();
    const density = profile.density;
    const radius = profile.radius;
    measureProfile(
      Float32Array.from([0, 1]),
      Float32Array.from([0, 0]),
      Float32Array.from([0, 0]),
      Float64Array.from([1, 1]),
      profile,
    );
    expect(profile.density).toBe(density);
    expect(profile.radius).toBe(radius);
  });
});
