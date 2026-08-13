/**
 * The machine every figure in this project was measured on, and the figures
 * this run produced on it.
 *
 * These are measurements rather than settings, so they are not in the
 * configuration file and cannot be parsed out of it. Each one is transcribed
 * from the document named beside it, and no figure appears here that those
 * documents do not state.
 */

export interface Fact {
  readonly name: string;
  readonly value: string;
  readonly unit?: string;
}

/** docs/performance.md, "The machine" and "What the machine can do". */
export const MACHINE: readonly Fact[] = [
  { name: 'CPU', value: 'Core Ultra 5 238V' },
  { name: 'cores', value: '4 P + 4 E, no SMT' },
  { name: 'GPU', value: 'Arc 130V, 7 Xe' },
  { name: 'memory', value: 'LPDDR5X-8533' },
  { name: 'read bandwidth', value: '95.7', unit: 'GB/s' },
  { name: 'fma peak, GPU', value: '2781', unit: 'Gflop/s' },
  { name: 'host copies', value: '0' },
];

/**
 * What the demonstration measured, from the section of README.md that gives
 * the commands producing it.
 *
 * These figures belong to the collision run taken at twenty thousand particles
 * over six thousand steps, which is what the two --set overrides in that
 * section ask for, and not to the forty thousand steps at sixty thousand
 * particles the file states. A measurement is a property of a configuration,
 * so the configuration it was taken under is carried with it and shown beside
 * it rather than left to be assumed.
 */
export const MEASURED = {
  count: 20_000,
  steps: 6_000,
  /** Relative energy error over the run. */
  energyDrift: 3.3e-3,
  /** The merger virialising: two galaxies in balance becoming one. */
  virialStart: 0.94,
  virialEnd: 0.99,
  /** Milliseconds a step, median of timed trials. */
  stepTime: 20.4,
  /** Seconds the whole run takes. */
  wallClock: 122,
} as const;
