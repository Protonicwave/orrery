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
  readonly id: string;
  readonly title: string;
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
 * The collision, at a size that can be fetched over a domestic connection.
 *
 * Eight thousand particles rather than the sixty thousand
 * `examples/collision.orrery` asks for, and a frame every hundred steps rather
 * than every forty. Both are downloads rather than physics: the run is the
 * whole encounter, forty thousand steps of it, and what the two settings change
 * is that the trajectory is thirty-nine megabytes instead of one and a half
 * gigabytes. The tidal tails and the merger are what the picture is for and
 * both survive the smaller sample; the plate states the count it actually drew,
 * so nothing here is passed off as the figure the native renderer reaches.
 */
export const GALLERY: readonly GalleryRun[] = [
  {
    id: 'collision',
    title: 'Two disc galaxies on a bound encounter',
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
        because: 'a hundred samples, which is what the rail will plot',
      },
      {
        setting: 'output.checkpoint_stride',
        value: '0',
        because: 'a published run is never resumed',
      },
    ],
  },
];

/** Where a run's trajectory is served from, under the site's base. */
export function trajectoryUrl(base: string, run: GalleryRun): string {
  return `${base}gallery/${run.id}.otj`;
}

/** Where its diagnostics are served from. */
export function diagnosticsUrl(base: string, run: GalleryRun): string {
  return `${base}gallery/${run.id}.csv`;
}
