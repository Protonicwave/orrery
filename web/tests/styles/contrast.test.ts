import { describe, expect, it } from 'vitest';
import TOKENS from '../../src/styles/tokens.css?raw';

/**
 * The greys, checked rather than assumed.
 *
 * A near-black interface makes it easy to pick a grey that looks quiet enough
 * to be right and is illegible to somebody who is not sitting where the
 * designer was sitting. These are the ratios WCAG 2.2 asks for: 4.5:1 for
 * text at the sizes this interface sets, and 3:1 for a mark that identifies a
 * control or its state.
 */

function token(name: string): string {
  const match = new RegExp(`--${name}:\\s*(#[0-9a-f]{6})`, 'i').exec(TOKENS);
  if (match?.[1] === undefined) throw new Error(`no --${name} in tokens.css`);
  return match[1];
}

function luminance(hex: string): number {
  const channels = [1, 3, 5].map(
    (at) => Number.parseInt(hex.slice(at, at + 2), 16) / 255,
  );
  const [red, green, blue] = channels.map((channel) =>
    channel <= 0.04045 ? channel / 12.92 : ((channel + 0.055) / 1.055) ** 2.4,
  ) as [number, number, number];
  return 0.2126 * red + 0.7152 * green + 0.0722 * blue;
}

function contrast(a: string, b: string): number {
  const [light, dark] = [luminance(a), luminance(b)].sort((x, y) => y - x) as [
    number,
    number,
  ];
  return (light + 0.05) / (dark + 0.05);
}

const SURFACES = ['chrome', 'chrome-recessed'];

describe('every grey that carries text', () => {
  for (const ink of ['ink', 'ink-2', 'ink-3']) {
    for (const surface of SURFACES) {
      it(`--${ink} reads on --${surface}`, () => {
        expect(contrast(token(ink), token(surface))).toBeGreaterThanOrEqual(4.5);
      });
    }
  }

  it('--ink-3 reads on the plate, which is where the plate notes are set', () => {
    expect(contrast(token('ink-3'), token('plate'))).toBeGreaterThanOrEqual(4.5);
  });

  it('brass reads as text, since it labels the tier and the priced control', () => {
    expect(contrast(token('brass'), token('chrome-recessed'))).toBeGreaterThanOrEqual(
      4.5,
    );
  });
});

describe('every mark that identifies a control', () => {
  for (const mark of ['edge', 'brass-dim', 'brass']) {
    for (const surface of SURFACES) {
      it(`--${mark} is visible against --${surface}`, () => {
        expect(contrast(token(mark), token(surface))).toBeGreaterThanOrEqual(3);
      });
    }
  }
});

describe('the reading half', () => {
  it('sets its text on paper at the same ratio the instrument holds', () => {
    expect(contrast(token('paper-ink'), token('paper'))).toBeGreaterThanOrEqual(4.5);
    expect(contrast(token('paper-ink-2'), token('paper'))).toBeGreaterThanOrEqual(4.5);
  });
});

describe('the furniture', () => {
  it('is quiet, and is therefore never allowed to carry text', () => {
    // --ink-4 draws registration marks, tick marks and a sparkline's zero
    // line. This asserts that it stays too quiet to be text, so that the day
    // somebody sets a word in it, this fails rather than a reader does.
    expect(contrast(token('ink-4'), token('chrome'))).toBeLessThan(3);
  });
});
