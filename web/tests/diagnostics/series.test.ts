import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import {
  type Diagnostics,
  DiagnosticsError,
  extent,
  parseDiagnostics,
  sampleAt,
} from '../../src/diagnostics/series';

/**
 * The fixture is a file the C++ wrote, not one written here.
 *
 * A reader tested against text its own test invented would agree with whatever
 * that test happened to type. This is six samples of examples/kepler.orrery,
 * produced by the single-precision build with
 *
 *     orrery run examples/kepler.orrery --set run.steps=1000 \
 *         --set output.diagnostics_stride=200 \
 *         --set output.diagnostics_path=web/tests/fixtures/kepler.csv
 *
 * and every figure asserted below is read out of it.
 */
const FIXTURE = readFileSync(
  join(dirname(fileURLToPath(import.meta.url)), '..', 'fixtures', 'kepler.csv'),
  'utf8',
);

describe('parseDiagnostics', () => {
  const kepler = parseDiagnostics(FIXTURE);

  it('reads a file the simulator wrote', () => {
    expect(kepler.samples).toBe(6);
    expect(Array.from(kepler.step)).toEqual([0, 200, 400, 600, 800, 1000]);
    expect(kepler.time[0]).toBe(0);
    expect(kepler.time[5]).toBeCloseTo(22.2144146, 6);
  });

  it('reads the columns the run measured', () => {
    expect(kepler.energyDrift[0]).toBe(0);
    expect(kepler.energyDrift[5]).toBeCloseTo(5.26905176e-4, 12);
    expect(kepler.virialRatio[0]).toBeCloseTo(1.50000012, 7);
    expect(kepler.virialRatio[5]).toBeCloseTo(1.48550403, 7);
  });

  /**
   * A two-body orbit released in the centre-of-mass frame has no linear
   * momentum at all, and its angular momentum is a constant of the motion. The
   * file says both, and this is what says the two magnitudes are formed from
   * the right three columns each. The tolerance is the single-precision
   * build's: the fixture's own column moves in its seventh digit.
   */
  it('forms a magnitude from the three components of a vector', () => {
    expect(Array.from(kepler.linearMomentum)).toEqual([0, 0, 0, 0, 0, 0]);
    for (const value of kepler.angularMomentum) {
      expect(value).toBeCloseTo(0.612372458, 5);
    }
  });

  /** The order in the file is the C++ writer's, and nothing may depend on it. */
  it('finds columns by name rather than by position', () => {
    const reversed = FIXTURE.split(/\r?\n/)
      .filter((line) => line !== '')
      .map((line) => line.split(',').reverse().join(','))
      .join('\n');

    const read = parseDiagnostics(reversed);
    expect(read.samples).toBe(kepler.samples);
    expect(Array.from(read.virialRatio)).toEqual(Array.from(kepler.virialRatio));
  });

  it('accepts a file whose lines end in carriage returns', () => {
    const windows = FIXTURE.replace(/\n/g, '\r\n');
    expect(parseDiagnostics(windows).samples).toBe(6);
  });

  it('refuses a file with a column missing', () => {
    const withoutVirial = FIXTURE.split(/\r?\n/)
      .filter((line) => line !== '')
      .map((line) => {
        const fields = line.split(',');
        fields.splice(6, 1);
        return fields.join(',');
      })
      .join('\n');

    expect(() => parseDiagnostics(withoutVirial)).toThrow(DiagnosticsError);
    expect(() => parseDiagnostics(withoutVirial)).toThrow(/virial_ratio/);
  });

  /**
   * The case this exists for: a run killed while writing its last line. Half a
   * sample is not a sample, and a plot drawn through one would be read as a
   * measurement of something that did not happen.
   */
  it('refuses a row that stops in the middle', () => {
    const cut = `${FIXTURE.trimEnd().slice(0, -20)}\n`;
    expect(() => parseDiagnostics(cut)).toThrow(DiagnosticsError);
  });

  it('refuses a field that is not a number', () => {
    const spoiled = FIXTURE.replace('1.48550403', 'nearly');
    expect(() => parseDiagnostics(spoiled)).toThrow(/not a number/);
  });

  it('reports the line a fault is on', () => {
    const spoiled = FIXTURE.replace('1.49763381', '');
    try {
      parseDiagnostics(spoiled);
      expect.unreachable('a blank field should have been refused');
    } catch (error) {
      expect(error).toBeInstanceOf(DiagnosticsError);
      // The heading is line one and the faulty sample is the third row.
      expect((error as DiagnosticsError).line).toBe(4);
    }
  });
});

describe('sampleAt', () => {
  const kepler = parseDiagnostics(FIXTURE);

  it('finds the last sample at or before a moment', () => {
    expect(sampleAt(kepler, 0)).toBe(0);
    expect(sampleAt(kepler, 4.44288301)).toBe(1);
    expect(sampleAt(kepler, 4.5)).toBe(1);
    expect(sampleAt(kepler, 8.88576602)).toBe(1);
    expect(sampleAt(kepler, 22.3)).toBe(5);
  });

  /** A run has a state before its first sample: the state it started from. */
  it('answers the first sample before the run begins', () => {
    expect(sampleAt(kepler, -1)).toBe(0);
  });

  it('answers nothing when there is nothing to answer with', () => {
    const empty: Diagnostics = {
      samples: 0,
      step: new Float64Array(0),
      time: new Float64Array(0),
      energyDrift: new Float64Array(0),
      virialRatio: new Float64Array(0),
      angularMomentum: new Float64Array(0),
      linearMomentum: new Float64Array(0),
    };
    expect(sampleAt(empty, 1)).toBe(-1);
  });
});

describe('extent', () => {
  it('is the smallest and the largest', () => {
    expect(extent(Float64Array.from([3, -1, 2]))).toEqual({ low: -1, high: 3 });
  });

  it('is nothing at all for an empty column', () => {
    expect(extent(new Float64Array(0))).toEqual({ low: 0, high: 0 });
  });
});
