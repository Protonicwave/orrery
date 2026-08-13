/**
 * Which run, and which moment in it, the address names.
 *
 * A run and an instant in it are the two things worth sending someone, so they
 * are in the address rather than in memory: `?run=cluster&t=12.5` opens the
 * cluster and puts the transport at twelve and a half time units. Everything
 * else the interface holds is a preference rather than a subject, and an
 * address that carried the exposure and the sprite radius would be an address
 * nobody could read.
 *
 * Query parameters rather than a path or a fragment. A path needs the server to
 * rewrite unknown ones onto the client, which GitHub Pages will not do for a
 * project site; a fragment is not sent to the server at all, which is fine, but
 * `?renderer=webgl2` is already a query parameter and two mechanisms for one
 * job is one too many.
 */

import { type GalleryRun, runById } from './runs';

export interface Address {
  readonly run: GalleryRun;
  /** The moment in model time, or null where the address names none. */
  readonly time: number | null;
}

/** What an address says. Anything it does not say is left to its default. */
export function readAddress(search: string): Address {
  const parameters = new URLSearchParams(search);
  const asked = parameters.get('t');
  const time = asked === null ? Number.NaN : Number(asked);
  return {
    run: runById(parameters.get('run')),
    // A moment that is not a number, or is before the run starts, names no
    // moment. The run then opens where a run opens, which is at its beginning.
    time: Number.isFinite(time) && time >= 0 ? time : null,
  };
}

/**
 * The address for a run at a moment, keeping whatever else is already there.
 *
 * `?renderer=` is set by hand to compare the two backends, and rewriting the
 * address as a run is selected must not throw that away.
 */
export function writeAddress(search: string, run: GalleryRun, time: number): string {
  const parameters = new URLSearchParams(search);
  parameters.set('run', run.id);
  // Three decimals, which is what the transport's clock shows. A full double
  // in an address is fifteen digits of a number a person is going to read.
  parameters.set('t', time.toFixed(3));
  return `?${parameters.toString()}`;
}
