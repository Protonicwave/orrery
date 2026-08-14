/**
 * The configuration a submitted run is given, derived from the one on screen.
 *
 * The same shape as `solver/configure.ts`, and for the same reason: everything
 * that makes the submitted run different from the published one is decided in
 * one pure function, so the note under the control can state exactly what was
 * changed. A run adjusted somewhere nobody can see is a picture nobody can
 * account for.
 *
 * Nothing here duplicates the service's rules. The service is the authority on
 * what it will take and refuses anything outside it, naming the setting; this
 * reads the ceilings it publishes so that the console cannot offer a run that
 * is certain to be refused. The two are not the same check: this one keeps the
 * controls honest, and that one keeps the service safe.
 */

import { type Configuration, override, writeConfiguration } from '../config/parse';
import type { Run } from '../config/run';
import type { Limits } from './contract';

export interface ServiceRun {
  /** The text submitted. */
  readonly text: string;
  /** Particles the run will integrate. */
  readonly count: number;
  /** Particles the configuration asked for, where it asks. */
  readonly requested: number | undefined;
  readonly steps: number;
  /** Whether the count or the step count had to be brought down to fit. */
  readonly reduced: boolean;
}

/**
 * The most steps a run of `count` particles may take within the work ceiling.
 *
 * The service scores work as the particle count times its logarithm times the
 * steps for a tree solver, and as the square of the count for a direct one.
 * Both are read from the ceiling it publishes rather than assumed, so a service
 * that raises or lowers it changes what this offers without the site being
 * rebuilt.
 */
export function affordableSteps(count: number, solver: string, limits: Limits): number {
  const perStep = solver === 'direct' ? count * count : count * Math.log2(count);
  if (perStep <= 0) return limits.max_steps;
  return Math.min(limits.max_steps, Math.floor(limits.max_work / perStep));
}

/**
 * The run the console would submit, given what its controls are set to.
 *
 * The output section is removed rather than replaced. Where a run writes is the
 * service's business, and a submission that stated one would be refused, which
 * is the right behaviour for a document that asked for something it cannot have.
 */
export function servicePlan(
  configuration: Configuration,
  requested: number | undefined,
  wanted: number,
  solver: string,
  settings: {
    readonly count: number;
    readonly softening: number;
    readonly integrator: string;
  },
  limits: Limits,
): ServiceRun {
  const count =
    requested === undefined
      ? undefined
      : Math.min(settings.count, limits.max_particles);
  const steps = Math.min(wanted, affordableSteps(count ?? 2, solver, limits));

  const configured = override(configuration, [
    { setting: 'run.steps', value: String(steps) },
    { setting: 'solver.softening', value: String(settings.softening) },
    { setting: 'integrator.kind', value: settings.integrator },
    ...(count === undefined
      ? []
      : [{ setting: 'initial_conditions.count', value: String(count) }]),
  ]);

  const stripped: Configuration = Object.fromEntries(
    Object.entries(configured).filter(([section]) => section !== 'output'),
  );

  return {
    text: writeConfiguration(stripped),
    // A configuration with no count is a scenario whose count is fixed by the
    // physics, and the Kepler problem's two bodies are the only such case here.
    count: count ?? 2,
    requested,
    steps,
    reduced: steps < wanted || (count !== undefined && count !== settings.count),
  };
}

/** The submitted version of the run the instrument is showing. */
export function serviceRun(
  run: Run,
  settings: {
    readonly count: number;
    readonly softening: number;
    readonly integrator: string;
  },
  limits: Limits,
): ServiceRun {
  return servicePlan(
    run.configuration,
    run.count,
    run.steps,
    run.solver,
    settings,
    limits,
  );
}
