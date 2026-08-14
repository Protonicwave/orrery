/**
 * What the priced button rests on.
 *
 * The estimate is one measured step time scaled along the tree solver's curve.
 * Which measured step time is the whole question, and it is the property worth
 * a test: before the service has run anything the only measurement this
 * repository has belongs to a different machine, and afterwards there is one
 * belonging to the machine that would actually take the run.
 */

import { describe, expect, it } from 'vitest';
import { estimateSeconds } from '../../src/components/Console';
import { MEASURED } from '../../src/data/machine';
import type { Reference } from '../../src/service/contract';

describe('the estimate', () => {
  it('reproduces the measured run it is scaled from', () => {
    // At the count and step count the demonstration was taken at, the estimate
    // is the wall clock that demonstration reported. Anything else would mean
    // the curve was not anchored to the measurement.
    const seconds = estimateSeconds(MEASURED.count, MEASURED.steps, null);
    expect(seconds).toBeCloseTo((MEASURED.stepTime * MEASURED.steps) / 1000, 6);
    expect(seconds).toBeCloseTo(MEASURED.wallClock, -1);
  });

  it('scales as the tree solver scales', () => {
    // Twice the particles is a little more than twice the work, because the
    // logarithm grows too. Not four times, which is what the direct solver
    // would cost and what a reader might otherwise assume.
    const single = estimateSeconds(10_000, 1_000, null);
    const double = estimateSeconds(20_000, 1_000, null);
    const ratio = double / single;

    expect(ratio).toBeGreaterThan(2);
    expect(ratio).toBeLessThan(2.2);
  });

  it('is linear in the steps', () => {
    expect(estimateSeconds(8_000, 2_000, null)).toBeCloseTo(
      2 * estimateSeconds(8_000, 1_000, null),
      6,
    );
  });

  it('uses the service’s own measurement once it has one', () => {
    // A service on slower hardware than the laptop the reports name. The
    // estimate has to follow the machine that would take the run rather than
    // the one the documents were written about.
    const slower: Reference = { step_ms: 80, particles: 20_000, jobs: 5 };

    const laptop = estimateSeconds(20_000, 1_000, null);
    const service = estimateSeconds(20_000, 1_000, slower);

    expect(laptop).toBeCloseTo((MEASURED.stepTime * 1_000) / 1000, 6);
    expect(service).toBeCloseTo(80, 6);
    expect(service).toBeGreaterThan(laptop);
  });

  it('scales the service’s measurement to a different size of run', () => {
    // The median the service reports was taken at whatever size its jobs
    // happened to be, so it is a point on the curve rather than the answer.
    const reference: Reference = { step_ms: 10, particles: 10_000, jobs: 3 };
    const same = estimateSeconds(10_000, 100, reference);
    const larger = estimateSeconds(20_000, 100, reference);

    expect(same).toBeCloseTo(1, 6);
    expect(larger / same).toBeGreaterThan(2);
  });
});
