import collisionText from '../../../examples/collision.orrery?raw';
import {
  type Configuration,
  identifier,
  numeric,
  parseConfiguration,
  setting,
} from './parse';

/**
 * The run the shell is showing.
 *
 * The text of examples/collision.orrery is read at build time and parsed here,
 * so every figure in the interface comes from the file the simulator would be
 * given. Nothing about the run is transcribed, and there is nothing to keep in
 * step by hand.
 */
export interface Run {
  /** Where the configuration lives in the repository. */
  readonly path: string;
  /** Seed and a hash of the file, which together name the run. */
  readonly name: string;
  readonly configuration: Configuration;
  readonly seed: number;
  readonly timestep: number;
  readonly steps: number;
  /** Steps times timestep: the interval of model time the run covers. */
  readonly modelTime: number;
  readonly count: number;
  readonly solver: string;
  readonly integrator: string;
  /** Frames the trajectory holds, and diagnostics samples beside them. */
  readonly frames: number;
  readonly samples: number;
}

function required(value: number | undefined, what: string): number {
  if (value === undefined) {
    throw new Error(`${what} is not set, and the shell has no default for it`);
  }
  return value;
}

function read(text: string, path: string): Run {
  const configuration = parseConfiguration(text);
  const seed = required(numeric(configuration, 'run', 'seed'), 'run.seed');
  const timestep = required(numeric(configuration, 'run', 'timestep'), 'run.timestep');
  const steps = required(numeric(configuration, 'run', 'steps'), 'run.steps');
  const stride = required(
    numeric(configuration, 'output', 'trajectory_stride'),
    'output.trajectory_stride',
  );
  const diagnostics = required(
    numeric(configuration, 'output', 'diagnostics_stride'),
    'output.diagnostics_stride',
  );

  return {
    path,
    name: identifier(text, seed),
    configuration,
    seed,
    timestep,
    steps,
    modelTime: steps * timestep,
    count: required(numeric(configuration, 'initial_conditions', 'count'), 'count'),
    solver: setting(configuration, 'solver', 'kind') ?? 'barnes-hut',
    integrator: setting(configuration, 'integrator', 'kind') ?? 'velocity-verlet',
    frames: Math.floor(steps / stride) + 1,
    samples: Math.floor(steps / diagnostics) + 1,
  };
}

export const collision: Run = read(collisionText, 'examples/collision.orrery');
