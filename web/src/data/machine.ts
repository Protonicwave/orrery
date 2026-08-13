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
 *
 * What is here is only what a run's own output cannot say. The energy drift and
 * the virial ratio used to be transcribed here too, and are not any more: the
 * instrument reads them out of the diagnostics file of the run it is showing,
 * which is a figure belonging to that run rather than to a different one
 * (ADR-0048). A step time and a wall clock are properties of the machine, the
 * diagnostics file carries neither, and they stay.
 */
export const MEASURED = {
  count: 20_000,
  steps: 6_000,
  /** Milliseconds a step, median of timed trials. */
  stepTime: 20.4,
  /** Seconds the whole run takes. */
  wallClock: 122,
} as const;
