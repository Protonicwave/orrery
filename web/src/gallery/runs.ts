/**
 * The published runs, defined once.
 *
 * This module is the only statement of what the gallery holds. The tool in
 * `web/tools/publish_gallery.ts` reads it to decide which simulations to run
 * and what to call their output; the client reads it to know what to fetch and
 * what to say about it. A run cannot therefore be published under one set of
 * settings and described under another.
 *
 * What is not here is anything the trajectory itself says. The particle count,
 * the timestep, the number of frames and the precision are read out of the
 * file's own header when it is opened, so the interface states what the run
 * actually is rather than what it was asked to be. That is also why there is no
 * sidecar index beside the trajectory: the format carries no frame count on
 * purpose, and it needs none, because every frame is the same length and the
 * count is the file's length divided by it. `web/src/trajectory/format.ts`
 * sets out the arithmetic.
 */

/** A setting the published run changes from the configuration file's own. */
export interface Override {
  /** `section.key`, as `orrery --set` takes it. */
  readonly setting: string;
  readonly value: string;
  /** Why this run differs from the file. Shown beside the run. */
  readonly because: string;
}

export interface GalleryRun {
  /** What the run is called in an address, and what its files are named. */
  readonly id: string;
  readonly title: string;
  /** One sentence: what this run is for, and what to watch in it. */
  readonly about: string;
  /** The configuration file, relative to the repository root. */
  readonly configuration: string;
  readonly overrides: readonly Override[];
  /**
   * Which build writes it.
   *
   * Single, for every published run. A trajectory in single precision is half
   * the download of the same run in double, and the renderer converts to single
   * on the way to the device regardless, so the second half would be bytes
   * fetched in order to be discarded. The physics is unaffected: precision is a
   * build-time choice the simulator makes for its own arithmetic (ADR-0006),
   * and what is written here is the result of that arithmetic rounded to what
   * a graphics device can hold.
   */
  readonly precision: 'single' | 'double';
}

/**
 * The three published runs, in the order the argument goes.
 *
 * Kepler first because it is the only gravitational system with a closed
 * solution and therefore the only one whose picture can be checked by looking
 * at it; the cluster next because it is an equilibrium the samplers are
 * validated against; the collision last because it is the one the project
 * exists to draw. `docs/validation.md` is the same argument at length, and the
 * gallery is arranged to be read in that order rather than by size.
 *
 * Every run is written at a frame every so many steps chosen to give about four
 * hundred frames, which is about seven seconds of playback, and about a hundred
 * diagnostics samples, which is what the rail plots. Those two settings are
 * downloads rather than physics; where a run changes a setting that is physics,
 * the reason is stated beside it and shown in the interface.
 */
export const GALLERY: readonly GalleryRun[] = [
  {
    id: 'kepler',
    title: 'Two bodies on an eccentric orbit',
    about:
      'The only gravitational system with a closed solution, and so the ' +
      'project’s primary validation instrument. Watch the energy: velocity ' +
      'Verlet holds it inside an envelope rather than driving it one way.',
    configuration: 'examples/kepler.orrery',
    precision: 'single',
    overrides: [
      {
        setting: 'run.steps',
        value: '2000',
        because: 'ten revolutions, which is enough to see that the orbit closes',
      },
      {
        setting: 'output.trajectory_stride',
        value: '5',
        because: 'forty frames a revolution',
      },
      {
        setting: 'output.diagnostics_stride',
        value: '20',
        because: 'a hundred samples, which is what the rail plots',
      },
    ],
  },
  {
    id: 'cluster',
    title: 'A Plummer sphere in equilibrium',
    about:
      'Four thousand particles sampled from a self-consistent model, so a ' +
      'correct sample stays in balance. The virial ratio is the one to read: ' +
      'it starts at one and it is meant to stay there.',
    configuration: 'examples/cluster.orrery',
    precision: 'single',
    overrides: [
      {
        setting: 'run.steps',
        value: '20000',
        because: 'twenty time units, which is several crossing times',
      },
      {
        setting: 'output.trajectory_stride',
        value: '50',
        because: 'four hundred frames, which is seven seconds of playback',
      },
      {
        setting: 'output.diagnostics_stride',
        value: '200',
        because: 'a hundred samples, which is what the rail plots',
      },
      {
        setting: 'output.checkpoint_stride',
        value: '0',
        because: 'a published run is never resumed',
      },
    ],
  },
  /**
   * The collision, at a size that can be fetched over a domestic connection.
   *
   * Eight thousand particles rather than the sixty thousand
   * `examples/collision.orrery` asks for, and a frame every hundred steps
   * rather than every forty. Both are downloads rather than physics: the run is
   * the whole encounter, forty thousand steps of it, and what the two settings
   * change is that the trajectory is thirty-nine megabytes instead of one and a
   * half gigabytes. The tidal tails and the merger are what the picture is for
   * and both survive the smaller sample; the plate states the count it actually
   * drew, so nothing here is passed off as the figure the native renderer
   * reaches.
   */
  {
    id: 'collision',
    title: 'Two disc galaxies on a bound encounter',
    about:
      'The demonstration: a grazing encounter that draws out one long tidal ' +
      'tail and one stubby one, and then merges. The energy drift is the ' +
      'price of forty thousand steps of it.',
    configuration: 'examples/collision.orrery',
    precision: 'single',
    overrides: [
      {
        setting: 'initial_conditions.count',
        value: '8000',
        because: 'a frame is a component array, so the count is the download',
      },
      {
        setting: 'run.steps',
        value: '40000',
        because: 'the whole encounter, as the configuration file asks for',
      },
      {
        setting: 'output.trajectory_stride',
        value: '100',
        because: 'four hundred frames, which is seven seconds of playback',
      },
      {
        setting: 'output.diagnostics_stride',
        value: '400',
        because: 'a hundred samples, which is what the rail plots',
      },
      {
        setting: 'output.checkpoint_stride',
        value: '0',
        because: 'a published run is never resumed',
      },
    ],
  },
];

/** The run an address names, or the one the instrument opens with. */
export function runById(id: string | null): GalleryRun {
  const found = GALLERY.find((run) => run.id === id);
  // The collision, because it is the run the repository demonstrates and the
  // one worth arriving at. An address naming a run that does not exist opens
  // the same one rather than an error page.
  return found ?? (GALLERY[GALLERY.length - 1] as GalleryRun);
}

/** Where a run's trajectory is served from, under the site's base. */
export function trajectoryUrl(base: string, run: GalleryRun): string {
  return `${base}gallery/${run.id}.otj`;
}

/** Where its diagnostics are served from. */
export function diagnosticsUrl(base: string, run: GalleryRun): string {
  return `${base}gallery/${run.id}.csv`;
}
