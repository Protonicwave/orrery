import { describe, expect, it } from 'vitest';
import { RUNS } from '../../src/config/run';
import { GALLERY } from '../../src/gallery/runs';

/**
 * The gallery's definition against the configuration files it names.
 *
 * Every published run is a repository configuration with a few settings
 * changed, and the tool that runs the simulator and the client that plays the
 * result both read this one definition. What can still go wrong is a run being
 * defined with a setting that does not produce a playable file, and that is
 * what these check: not that the numbers are the ones chosen, but that they
 * are numbers a run can be made and shown from.
 */
describe('the published runs', () => {
  it('is a run for every entry, read from the file it names', () => {
    expect(RUNS.length).toBe(GALLERY.length);
    for (const run of RUNS) {
      expect(run.path).toBe(run.published.configuration);
      expect(run.path.startsWith('examples/')).toBe(true);
    }
  });

  it('gives every run an identifier that can go in an address', () => {
    const ids = GALLERY.map((run) => run.id);
    expect(new Set(ids).size).toBe(ids.length);
    for (const id of ids) expect(id).toMatch(/^[a-z][a-z0-9-]*$/);
  });

  /**
   * About four hundred frames each, which is about seven seconds of playback,
   * and about a hundred diagnostics samples, which is what the rail plots. The
   * bounds are loose because the figures are chosen per run; what they catch is
   * a stride that would produce eleven frames or forty thousand.
   */
  it('is written at a stride that gives a run worth watching', () => {
    for (const run of RUNS) {
      expect(run.frames).toBeGreaterThan(100);
      expect(run.frames).toBeLessThanOrEqual(1000);
      expect(run.samples).toBeGreaterThan(20);
      expect(run.samples).toBeLessThanOrEqual(500);
    }
  });

  /** Steps times timestep, and the transport's whole range. */
  it('covers an interval of model time the transport can span', () => {
    for (const run of RUNS) {
      expect(run.modelTime).toBeCloseTo(run.steps * run.timestep, 9);
      expect(run.modelTime).toBeGreaterThan(0);
    }
  });

  /**
   * A published run is never resumed, so nothing should be writing checkpoints
   * of it. A configuration that asks for them costs the publishing job the time
   * to write a checkpoint of every particle every so many steps, for a file
   * that is then thrown away.
   */
  it('writes no checkpoints', () => {
    for (const run of RUNS) {
      const stride = run.configuration.output?.checkpoint_stride;
      if (stride !== undefined) expect(Number(stride)).toBe(0);
    }
  });

  it('states why each setting differs from the file it came from', () => {
    for (const run of GALLERY) {
      for (const override of run.overrides) {
        expect(override.because.length).toBeGreaterThan(10);
        expect(override.setting).toContain('.');
      }
    }
  });
});
