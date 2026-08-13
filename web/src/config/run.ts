import collisionText from '../../../examples/collision.orrery?raw';
import { GALLERY, type GalleryRun } from '../gallery/runs';
import {
  type Configuration,
  identifier,
  numeric,
  override,
  parseConfiguration,
  setting,
} from './parse';

/**
 * The run the instrument is showing.
 *
 * The text of examples/collision.orrery is read at build time and parsed here,
 * then the published run's overrides are applied to it, exactly as the
 * simulator applies them on the command line. So every figure in the interface
 * is the figure the run was given: nothing is transcribed, nothing is kept in
 * step by hand, and the panels cannot disagree with the plate about how many
 * particles were integrated.
 *
 * What the trajectory itself says is read from the trajectory. The two are
 * expected to agree, and where the interface has both, it shows the file's:
 * the plate's particle count comes from the header of the trajectory being
 * drawn, and the rail's comes from the configuration that produced it.
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
  /**
   * The precision the published trajectory was written in.
   *
   * A property of the build that produced it rather than of the configuration,
   * which is why it comes from the gallery's own definition. The masthead
   * states it because a browser drawing a run in single precision must not be
   * read as the double-precision figure the reports quote.
   */
  readonly precision: 'f32' | 'f64';
}

function required(value: number | undefined, what: string): number {
  if (value === undefined) {
    throw new Error(`${what} is not set, and the shell has no default for it`);
  }
  return value;
}

function read(text: string, path: string, published: GalleryRun | undefined): Run {
  const configuration = override(parseConfiguration(text), published?.overrides ?? []);
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
    // The name is a hash of the configuration file, not of the run made from
    // it. It identifies the scenario, and the overrides are stated beside it.
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
    precision: published?.precision === 'double' ? 'f64' : 'f32',
  };
}

export const collision: Run = read(
  collisionText,
  'examples/collision.orrery',
  GALLERY.find((run) => run.id === 'collision'),
);
