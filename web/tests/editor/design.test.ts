import { describe, expect, it } from 'vitest';
import { numeric, parseConfiguration, setting } from '../../src/config/parse';
import { describeDesign } from '../../src/editor/description';
import {
  type Design,
  designFilename,
  PRESETS,
  preset,
  problemsWith,
  withKind,
  writeDesign,
} from '../../src/editor/design';

/**
 * The file the editor writes.
 *
 * What matters about it is that the C++ would accept it, so these check the two
 * things this side can check: that the client's own reader parses what the
 * writer produces, and that the settings that come back are the settings that
 * went in. Whether the native binary agrees is not a question a unit test can
 * answer, and `agreement.test.ts` answers it by running one.
 */

function written(design: Design): string {
  return writeDesign(design, describeDesign);
}

describe('every preset', () => {
  it.each(PRESETS.map((entry) => entry.id))(
    'is a configuration the reader accepts: %s',
    (kind) => {
      expect(problemsWith(preset(kind))).toEqual([]);
    },
  );

  it.each(PRESETS.map((entry) => entry.id))(
    'parses back to what it states: %s',
    (kind) => {
      const design = preset(kind);
      const configuration = parseConfiguration(written(design));

      expect(setting(configuration, 'initial_conditions', 'kind')).toBe(kind);
      expect(numeric(configuration, 'run', 'timestep')).toBe(design.timestep);
      expect(numeric(configuration, 'run', 'steps')).toBe(design.steps);
      expect(numeric(configuration, 'run', 'seed')).toBe(design.seed);
      expect(setting(configuration, 'solver', 'kind')).toBe(design.solver);
      expect(numeric(configuration, 'solver', 'softening')).toBe(design.softening);
      expect(setting(configuration, 'integrator', 'kind')).toBe(design.integrator);
    },
  );

  it.each(PRESETS.map((entry) => entry.id))('is written in ASCII: %s', (kind) => {
    // The C++ reader takes bytes and the file is read by whoever opens it in an
    // editor. A minus sign that is not a hyphen belongs in the interface, where
    // the typography is doing measurement work, and not here.
    // biome-ignore lint/suspicious/noControlCharactersInRegex: the range is the point
    expect(written(preset(kind))).toMatch(/^[\x09\x0a\x20-\x7e]*$/);
  });
});

describe('the file a design writes', () => {
  it('states the count for a sampled configuration and not for the two-body one', () => {
    const collision = parseConfiguration(written(preset('galaxy-collision')));
    expect(numeric(collision, 'initial_conditions', 'count')).toBe(60000);

    // The reader rejects a count beside the kepler configuration rather than
    // ignoring one, so writing it would produce a file the C++ refuses.
    const kepler = parseConfiguration(written(preset('kepler')));
    expect(setting(kepler, 'initial_conditions', 'count')).toBeUndefined();
    expect(numeric(kepler, 'initial_conditions', 'eccentricity')).toBe(0.5);
  });

  it('states the encounter only where there is one', () => {
    const disc = parseConfiguration(written(preset('disc-galaxy')));
    expect(setting(disc, 'initial_conditions', 'separation')).toBeUndefined();
    expect(numeric(disc, 'initial_conditions', 'inclination')).toBe(0.35);

    const collision = parseConfiguration(written(preset('galaxy-collision')));
    expect(numeric(collision, 'initial_conditions', 'separation')).toBe(20);
    expect(numeric(collision, 'initial_conditions', 'impact_parameter')).toBe(2);
    expect(numeric(collision, 'initial_conditions', 'approach_speed')).toBe(0.8);
  });

  it('opens with a comment and nothing else', () => {
    const lines = written(preset('plummer')).split('\n');
    expect(lines[0]?.startsWith('# ')).toBe(true);
    const heading = lines.findIndex((line) => line.startsWith('['));
    expect(heading).toBeGreaterThan(0);
    for (const line of lines.slice(0, heading)) {
      expect(line === '' || line.startsWith('#')).toBe(true);
    }
  });

  it('names itself after the scenario and the seed', () => {
    expect(designFilename(preset('galaxy-collision'))).toBe(
      'collision-20260812.orrery',
    );
    expect(designFilename(preset('plummer'))).toBe('plummer-20260811.orrery');
  });
});

describe('moving along the path of presets', () => {
  it('keeps the count and takes the run settings', () => {
    const from = { ...preset('plummer'), count: 8192 };
    const to = withKind(from, 'galaxy-collision');

    expect(to.count).toBe(8192);
    expect(to.timestep).toBe(preset('galaxy-collision').timestep);
    expect(to.softening).toBe(preset('galaxy-collision').softening);
    expect(problemsWith(to)).toEqual([]);
  });

  it('drops the count when the scenario has no use for one, and finds it again', () => {
    const kepler = withKind(preset('galaxy-collision'), 'kepler');
    expect(kepler.count).toBe(0);
    expect(problemsWith(kepler)).toEqual([]);

    const back = withKind(kepler, 'disc-galaxy');
    expect(back.count).toBe(preset('disc-galaxy').count);
    expect(problemsWith(back)).toEqual([]);
  });
});

describe('what the reader would refuse', () => {
  it('is reported in the reader’s own words', () => {
    const design = preset('galaxy-collision');
    expect(problemsWith({ ...design, timestep: 0 })).toContain(
      'run.timestep must be a positive number',
    );
    expect(problemsWith({ ...design, massRatio: 1.5 })).toContain(
      'initial_conditions.mass_ratio must lie in (0, 1]',
    );
    expect(problemsWith({ ...design, count: 2 })).toContain(
      'initial_conditions.count must be at least four for a collision, which is two galaxies',
    );
    expect(problemsWith({ ...design, bulgeFraction: 1 })).toContain(
      'initial_conditions.bulge_fraction must lie in [0, 1)',
    );
    expect(problemsWith({ ...preset('kepler'), eccentricity: 1 })).toContain(
      'initial_conditions.eccentricity must lie in [0, 1) for a bound orbit',
    );
    expect(problemsWith({ ...preset('kepler'), count: 100 })).toContain(
      'initial_conditions.count is not used by the kepler configuration, which is always two bodies',
    );
  });

  it('includes a NaN, which a comparison written the other way round would let through', () => {
    const design = preset('disc-galaxy');
    expect(problemsWith({ ...design, scaleLength: Number.NaN })).toContain(
      'initial_conditions.scale_length must be a positive number',
    );
  });
});
