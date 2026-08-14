/**
 * What a submitted run is asked to be.
 *
 * The interesting cases are the reductions, as they are for a browser run. A
 * published run is tens of thousands of steps of tens of thousands of
 * particles, the service will take rather less than that, and the question this
 * answers is what is given up and whether the console can say so exactly.
 */

import { describe, expect, it } from 'vitest';
import { parseConfiguration, setting } from '../../src/config/parse';
import { RUNS } from '../../src/config/run';
import type { Limits } from '../../src/service/contract';
import { affordableSteps, serviceRun } from '../../src/service/request';

/** The ceilings the service actually publishes, so the cases are the real ones. */
const LIMITS: Limits = {
  max_particles: 20_000,
  max_steps: 20_000,
  max_work: 20_000 * Math.log2(20_000) * 6_000,
  max_queue: 32,
  solvers: ['direct', 'barnes-hut'],
};

const SETTINGS = {
  count: 8_000,
  softening: 0.12,
  integrator: 'velocity-verlet',
};

function runNamed(id: string) {
  const found = RUNS.find((run) => run.published.id === id);
  if (found === undefined) throw new Error(`${id} is not published`);
  return found;
}

describe('a submitted run', () => {
  it('carries what the controls were set to', () => {
    const plan = serviceRun(
      runNamed('cluster'),
      { count: 6_000, softening: 0.08, integrator: 'yoshida4' },
      LIMITS,
    );
    const configuration = parseConfiguration(plan.text);

    expect(setting(configuration, 'initial_conditions', 'count')).toBe('6000');
    expect(setting(configuration, 'solver', 'softening')).toBe('0.08');
    expect(setting(configuration, 'integrator', 'kind')).toBe('yoshida4');
  });

  it('does not decide where the run writes', () => {
    // The service refuses a submission that states an output section, because
    // where a run writes is its business. Leaving the section out is what makes
    // the document one the service will take.
    const plan = serviceRun(runNamed('collision'), SETTINGS, LIMITS);
    expect(parseConfiguration(plan.text).output).toBeUndefined();
    expect(plan.text).not.toContain('trajectory_path');
  });

  it('cuts the particle count to what the service will take', () => {
    const plan = serviceRun(
      runNamed('collision'),
      { ...SETTINGS, count: 500_000 },
      LIMITS,
    );
    expect(plan.count).toBe(LIMITS.max_particles);
    expect(plan.reduced).toBe(true);
  });

  it('cuts the steps to what the work ceiling allows', () => {
    // The collision is forty thousand steps, which at any accepted count is
    // more work than the service takes in one run.
    const collision = runNamed('collision');
    const plan = serviceRun(collision, SETTINGS, LIMITS);

    expect(plan.steps).toBeLessThan(collision.steps);
    expect(plan.reduced).toBe(true);
    expect(setting(parseConfiguration(plan.text), 'run', 'steps')).toBe(
      String(plan.steps),
    );
  });

  it('says nothing was given up when nothing was', () => {
    // The cluster is a thousand steps of four thousand particles, which is well
    // inside every ceiling.
    const plan = serviceRun(runNamed('cluster'), { ...SETTINGS, count: 4_096 }, LIMITS);
    expect(plan.reduced).toBe(false);
    expect(plan.steps).toBe(runNamed('cluster').steps);
  });

  it('leaves a scenario whose count is fixed by the physics alone', () => {
    // The Kepler problem states two masses rather than a count, so there is no
    // count to override and the run is two bodies whatever the slider says.
    const plan = serviceRun(runNamed('kepler'), SETTINGS, LIMITS);
    expect(plan.count).toBe(2);
    expect(setting(parseConfiguration(plan.text), 'initial_conditions', 'count')).toBe(
      undefined,
    );
  });

  it('writes a configuration the reader accepts', () => {
    for (const run of RUNS) {
      const plan = serviceRun(run, SETTINGS, LIMITS);
      expect(() => parseConfiguration(plan.text)).not.toThrow();
    }
  });
});

describe('the steps a count can afford', () => {
  it('gives a small run the whole step ceiling', () => {
    expect(affordableSteps(1_000, 'barnes-hut', LIMITS)).toBe(LIMITS.max_steps);
  });

  it('gives the largest run the reference number of steps', () => {
    // The ceiling is the demonstration in README.md, which is that count over
    // six thousand steps, so the largest accepted run buys exactly that.
    expect(affordableSteps(20_000, 'barnes-hut', LIMITS)).toBe(6_000);
  });

  it('scores the direct solver on the curve it follows', () => {
    // Every pair rather than a tree walk, so the same count buys far fewer
    // steps. A ceiling that scored both the same would understate this one.
    const tree = affordableSteps(8_000, 'barnes-hut', LIMITS);
    const direct = affordableSteps(8_000, 'direct', LIMITS);
    expect(direct).toBeLessThan(tree);
  });
});
