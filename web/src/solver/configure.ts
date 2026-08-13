/**
 * The configuration a browser run is given, derived from the published one.
 *
 * A run in a browser is the same scenario as the published run and a different
 * size of it, and everything that makes it different is decided here, in one
 * pure function, so that the note under the control can state exactly what was
 * changed. The alternative, adjusting settings in the Worker where nobody can
 * see them, would produce a picture nobody could account for.
 *
 * Nothing here decides anything the module also decides. The module refuses a
 * count above its own limit and forces the run serial; this reduces the count so
 * that it is not refused, and says by how much.
 */

import { type Configuration, override, writeConfiguration } from '../config/parse';
import type { Run } from '../config/run';

/**
 * The most steps a browser run takes.
 *
 * The published runs are tens of thousands of steps, which at the step times
 * this build reaches is several minutes of a tab doing nothing else. Two
 * thousand is long enough to see a Plummer sphere hold its virial ratio and a
 * pair of discs begin to fall together, and short enough to watch. A run that
 * wants the whole encounter is the published one, which was integrated on the
 * machine the reports name.
 */
export const BROWSER_STEPS = 2000;

/**
 * The most frames a browser run keeps.
 *
 * Every frame is three component arrays held for the length of the run, so at
 * the module's particle limit four hundred frames is about twenty megabytes.
 * Keeping them is what makes the transport able to scrub back through what has
 * been computed rather than only follow the head of it.
 */
export const BROWSER_FRAMES = 400;

export interface BrowserRun {
  /** The text handed to the module. */
  readonly text: string;
  /** Particles the run will integrate. */
  readonly count: number;
  /** Particles the published configuration asked for, where it asks. */
  readonly requested: number | undefined;
  readonly steps: number;
  readonly stride: number;
  readonly frames: number;
  /** Whether the particle count or the step count had to be brought down. */
  readonly reduced: boolean;
}

/**
 * The configuration for a browser run of `run`, capped at `particleLimit`.
 *
 * The output section is emptied here as well as ignored by the module, so that
 * the document handed across is one that could be run anywhere and mean the
 * same thing. A configuration that names a trajectory path and is given to
 * something with no filesystem is a document that lies about what it does.
 */
export function browserRun(run: Run, particleLimit: number): BrowserRun {
  const requested = run.count;
  const count =
    requested === undefined ? undefined : Math.min(requested, particleLimit);
  const steps = Math.min(run.steps, BROWSER_STEPS);
  const stride = Math.max(1, Math.ceil(steps / BROWSER_FRAMES));

  const settings = [
    { setting: 'run.steps', value: String(steps) },
    ...(count === undefined
      ? []
      : [{ setting: 'initial_conditions.count', value: String(count) }]),
  ];

  const configured: Configuration = override(run.configuration, settings);
  const stripped: Configuration = Object.fromEntries(
    Object.entries(configured).filter(([section]) => section !== 'output'),
  );

  return {
    text: writeConfiguration(stripped),
    // A configuration with no count is a scenario whose count is fixed by the
    // physics, and the Kepler problem's two bodies are the only such case here.
    // What was actually produced is reported by the module and is what the plate
    // shows; this is what the note beside the control says it asked for.
    count: count ?? 2,
    requested,
    steps,
    stride,
    frames: Math.floor(steps / stride) + 1,
    reduced: steps < run.steps || (requested !== undefined && count !== requested),
  };
}
