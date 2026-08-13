/**
 * What a browser run is asked to be.
 *
 * The interesting cases are all reductions. A published run is tens of
 * thousands of steps of tens of thousands of particles and a tab is neither of
 * those, so the question this answers is what is given up and whether the note
 * beside the control can say so exactly.
 */

import { describe, expect, it } from 'vitest';
import { parseConfiguration, setting } from '../../src/config/parse';
import { RUNS } from '../../src/config/run';
import { BROWSER_FRAMES, BROWSER_STEPS, browserRun } from '../../src/solver/configure';

const LIMIT = 4096;

function runNamed(id: string) {
  const found = RUNS.find((run) => run.published.id === id);
  if (found === undefined) throw new Error(`${id} is not published`);
  return found;
}

describe('a browser run', () => {
  it('cuts the particle count to what the module will accept', () => {
    const collision = runNamed('collision');
    const plan = browserRun(collision, LIMIT);

    expect(collision.count).toBeGreaterThan(LIMIT);
    expect(plan.count).toBe(LIMIT);
    expect(plan.reduced).toBe(true);

    const configuration = parseConfiguration(plan.text);
    expect(setting(configuration, 'initial_conditions', 'count')).toBe(String(LIMIT));
  });

  it('leaves a count the module would accept alone', () => {
    const cluster = runNamed('cluster');
    const plan = browserRun(cluster, 1 << 20);
    expect(plan.count).toBe(cluster.count);
  });

  it('says two bodies for a configuration that states no count', () => {
    const kepler = runNamed('kepler');
    expect(kepler.count).toBeUndefined();

    const plan = browserRun(kepler, LIMIT);
    expect(plan.count).toBe(2);

    // Nothing is invented: a configuration that does not state a count is
    // handed on without one, because the count is a property of the scenario.
    const configuration = parseConfiguration(plan.text);
    expect(setting(configuration, 'initial_conditions', 'count')).toBeUndefined();
  });

  it('keeps the run short enough to watch', () => {
    for (const run of RUNS) {
      const plan = browserRun(run, LIMIT);
      expect(plan.steps).toBeLessThanOrEqual(BROWSER_STEPS);
      expect(plan.steps).toBeLessThanOrEqual(run.steps);
      expect(plan.frames).toBeLessThanOrEqual(BROWSER_FRAMES + 1);
      expect(plan.stride).toBeGreaterThanOrEqual(1);
    }
  });

  it('writes a document with nowhere to write output to', () => {
    for (const run of RUNS) {
      const configuration = parseConfiguration(browserRun(run, LIMIT).text);
      expect(configuration.output).toBeUndefined();
    }
  });

  it('writes a document the reader accepts unchanged', () => {
    for (const run of RUNS) {
      const plan = browserRun(run, LIMIT);
      const once = parseConfiguration(plan.text);
      const twice = parseConfiguration(plan.text);
      expect(twice).toEqual(once);

      // Every setting the published configuration had, but for the two that
      // were changed and the section that was dropped.
      expect(setting(once, 'run', 'steps')).toBe(String(plan.steps));
      expect(setting(once, 'run', 'seed')).toBe(String(run.seed));
      expect(setting(once, 'solver', 'kind')).toBe(run.solver);
    }
  });
});
