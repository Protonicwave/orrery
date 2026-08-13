import { describe, expect, it } from 'vitest';
import { plotRange } from '../../src/diagnostics/plot';

describe('plotRange', () => {
  it('leaves a margin above and below, so the line never touches the edge', () => {
    const { low, high } = plotRange(0, 1);
    expect(low).toBeLessThan(0);
    expect(high).toBeGreaterThan(1);
    expect(high - 1).toBeCloseTo(-low, 12);
  });

  /**
   * The case a conserved quantity produces. A run whose momentum is nothing
   * from beginning to end has a column of identical values, and dividing by
   * the height of that range would put every point at a NaN.
   */
  it('gives a column that never changes a range to be drawn in', () => {
    const { low, high } = plotRange(0.5, 0.5);
    expect(high).toBeGreaterThan(low);
    expect((low + high) / 2).toBeCloseTo(0.5, 12);
  });

  it('gives a column of zeroes a range as well', () => {
    const { low, high } = plotRange(0, 0);
    expect(high).toBeGreaterThan(low);
  });

  /** An empty column has no extent, and a plot of one draws nothing. */
  it('refuses to scale against something that is not a number', () => {
    expect(plotRange(Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY)).toEqual({
      low: 0,
      high: 1,
    });
  });
});
