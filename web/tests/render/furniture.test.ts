import { describe, expect, it } from 'vitest';
import { scaleLength, scaleText } from '../../src/render/furniture';

/**
 * The scale bar, which is the piece of furniture that can be wrong quietly.
 *
 * A gnomon that points the wrong way is obvious. A scale bar that says five
 * when it means fifty is a plausible picture of a galaxy at the wrong size, and
 * nothing on the plate would contradict it.
 */

describe('choosing what the scale bar stands for', () => {
  it('picks a length of one, two or five times a power of ten', () => {
    for (const perPixel of [0.3, 1, 2.5, 7, 19, 140, 1200, 0.004]) {
      const length = scaleLength(perPixel);
      expect(length).toBeGreaterThan(0);

      const mantissa = length / 10 ** Math.floor(Math.log10(length));
      expect([1, 2, 5]).toContainEqual(Math.round(mantissa));
    }
  });

  it('keeps the bar inside the width it is allowed', () => {
    for (const perPixel of [0.3, 1, 2.5, 7, 19, 140, 1200, 0.004]) {
      const pixels = scaleLength(perPixel) * perPixel;
      expect(pixels).toBeGreaterThanOrEqual(60);
      expect(pixels).toBeLessThanOrEqual(170);
    }
  });

  it('grows the length as the camera pulls back', () => {
    // Half the pixels per unit means twice the model length in the same bar,
    // give or take the step the scale is allowed to take.
    const near = scaleLength(20);
    const far = scaleLength(2);
    expect(far).toBeGreaterThan(near);
  });

  it('says nothing rather than something wrong at an impossible scale', () => {
    expect(scaleLength(0)).toBe(0);
    expect(scaleLength(-1)).toBe(0);
    expect(scaleLength(Number.NaN)).toBe(0);
    expect(scaleLength(Number.POSITIVE_INFINITY)).toBe(0);
    expect(scaleText(0)).toBe('');
  });
});

describe('writing the length on the bar', () => {
  it('sets whole numbers without a decimal point', () => {
    expect(scaleText(5)).toBe('5');
    expect(scaleText(20)).toBe('20');
  });

  it('groups the digits the way every other figure is grouped', () => {
    // A thin space, U+2009, as format/number.ts sets one.
    expect(scaleText(2000)).toBe('2 000');
  });

  it('keeps enough figures for a length below one to be a length', () => {
    expect(scaleText(0.5)).toBe('0.5');
    expect(scaleText(0.02)).toBe('0.02');
  });
});
