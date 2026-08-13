/**
 * A reader for the diagnostics file a run writes, to the specification in
 * docs/formats/ and to what `sim/diagnostics_log.cpp` actually emits.
 *
 * The instrument plots what the run measured rather than what the browser can
 * work out from the positions. It could do the second: the trajectory carries
 * the masses, so a kinetic energy needs only the velocities and a potential
 * energy needs only a solver. The reason it does not is that the answer would
 * then be the browser's answer, computed in single precision from a rounded
 * copy of the state at whatever stride the trajectory was written at, and the
 * whole point of the rail is to show what the run conserved. ADR-0048.
 *
 * Columns are found by name. The C++ writes them in a fixed order, but a
 * reader that depends on the order would break silently if a column were ever
 * inserted, and breaking loudly is cheaper here than a header lookup.
 */

/** Everything wrong with a file is reported the way the C++ reports it: by line. */
export class DiagnosticsError extends Error {
  readonly line: number;

  constructor(line: number, message: string) {
    super(`line ${line}: ${message}`);
    this.name = 'DiagnosticsError';
    this.line = line;
  }
}

/**
 * The columns the instrument reads, one array each.
 *
 * Typed arrays rather than an array of records: a hundred samples is nothing,
 * but a run written at a fine stride is tens of thousands, and a sparkline
 * walks every one of them each time it is drawn.
 */
export interface Diagnostics {
  readonly samples: number;
  readonly step: Float64Array;
  readonly time: Float64Array;
  /** The run's own relative energy error, against its first measured energy. */
  readonly energyDrift: Float64Array;
  /** −2T/U. One for a system in equilibrium. */
  readonly virialRatio: Float64Array;
  /** The magnitude of the total angular momentum. */
  readonly angularMomentum: Float64Array;
  /** The magnitude of the total linear momentum, which should stay at nothing. */
  readonly linearMomentum: Float64Array;
}

const REQUIRED = [
  'step',
  'time',
  'relative_energy_error',
  'virial_ratio',
  'momentum_x',
  'momentum_y',
  'momentum_z',
  'angular_momentum_x',
  'angular_momentum_y',
  'angular_momentum_z',
] as const;

function magnitude(x: number, y: number, z: number): number {
  return Math.sqrt(x * x + y * y + z * z);
}

/**
 * Parse the text of a diagnostics file.
 *
 * @throws DiagnosticsError if a required column is missing, a row has the
 * wrong number of fields, or a field is not a number. A file that is only
 * partly readable is not read partly: a run interrupted halfway through
 * writing a line would otherwise contribute one sample of rubbish to every
 * plot, and a plot is read as a measurement.
 */
export function parseDiagnostics(text: string): Diagnostics {
  const lines = text.split(/\r?\n/).filter((line) => line.trim() !== '');
  const heading = lines[0];
  if (heading === undefined) throw new DiagnosticsError(1, 'the file is empty');

  const names = heading.split(',').map((name) => name.trim());
  const column: Record<string, number> = {};
  for (const [index, name] of names.entries()) column[name] = index;

  for (const wanted of REQUIRED) {
    if (!(wanted in column)) {
      throw new DiagnosticsError(1, `there is no ${wanted} column`);
    }
  }

  const samples = lines.length - 1;
  const step = new Float64Array(samples);
  const time = new Float64Array(samples);
  const energyDrift = new Float64Array(samples);
  const virialRatio = new Float64Array(samples);
  const angularMomentum = new Float64Array(samples);
  const linearMomentum = new Float64Array(samples);

  for (let row = 0; row < samples; row += 1) {
    const number = row + 2;
    const fields = (lines[row + 1] as string).split(',');
    if (fields.length !== names.length) {
      throw new DiagnosticsError(
        number,
        `${fields.length} fields where the heading names ${names.length}`,
      );
    }

    const read = (name: string): number => {
      const text = (fields[column[name] as number] as string).trim();
      const value = Number(text);
      // Number("") is zero and Number("1 2") is NaN, so both are checked. The
      // C++ writes in the classic locale precisely so that this is enough.
      if (text === '' || !Number.isFinite(value)) {
        throw new DiagnosticsError(number, `${name} is not a number: "${text}"`);
      }
      return value;
    };

    step[row] = read('step');
    time[row] = read('time');
    energyDrift[row] = read('relative_energy_error');
    virialRatio[row] = read('virial_ratio');
    linearMomentum[row] = magnitude(
      read('momentum_x'),
      read('momentum_y'),
      read('momentum_z'),
    );
    angularMomentum[row] = magnitude(
      read('angular_momentum_x'),
      read('angular_momentum_y'),
      read('angular_momentum_z'),
    );
  }

  return {
    samples,
    step,
    time,
    energyDrift,
    virialRatio,
    angularMomentum,
    linearMomentum,
  };
}

/**
 * Fetch a run's diagnostics.
 *
 * On the main thread, unlike the trajectory. ADR-0047 moved the trajectory off
 * it because a trajectory is tens of megabytes decoded frame by frame while the
 * loop is drawing; a diagnostics file is a hundred lines and is read once,
 * before anything is being drawn from it.
 */
export async function fetchDiagnostics(url: string): Promise<Diagnostics> {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`${url}: the run's diagnostics answered ${response.status}`);
  }
  return parseDiagnostics(await response.text());
}

/**
 * The index of the last sample at or before a moment in model time.
 *
 * A binary search, because the transport asks this of four plots ten times a
 * second and the samples are in order by construction. Before the first sample
 * it answers zero: a run has a state at t = 0 and that state is the first row.
 */
export function sampleAt(times: Float64Array, time: number): number {
  if (times.length === 0) return -1;

  let low = 0;
  let high = times.length - 1;
  while (low < high) {
    const middle = (low + high + 1) >> 1;
    if ((times[middle] as number) <= time) low = middle;
    else high = middle - 1;
  }
  return low;
}

/** The smallest and largest of a column, for scaling a plot to its own data. */
export function extent(values: Float64Array): { low: number; high: number } {
  let low = Number.POSITIVE_INFINITY;
  let high = Number.NEGATIVE_INFINITY;
  for (const value of values) {
    if (value < low) low = value;
    if (value > high) high = value;
  }
  // An empty column has no extent, and a plot of one draws nothing rather than
  // a line through a pair of infinities.
  return low > high ? { low: 0, high: 0 } : { low, high };
}
