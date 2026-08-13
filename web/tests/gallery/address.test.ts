import { describe, expect, it } from 'vitest';
import { readAddress, writeAddress } from '../../src/gallery/address';
import { GALLERY } from '../../src/gallery/runs';

describe('readAddress', () => {
  it('reads the run and the moment', () => {
    const address = readAddress('?run=cluster&t=12.5');
    expect(address.run.id).toBe('cluster');
    expect(address.time).toBe(12.5);
  });

  /** An address with nothing in it is the one everybody arrives at. */
  it('opens the collision when the address names no run', () => {
    expect(readAddress('').run.id).toBe('collision');
    expect(readAddress('').time).toBeNull();
  });

  /**
   * A run that has been renamed, or an address somebody has typed. Showing the
   * demonstration is a better answer than an error page, and it is the answer
   * a reader following an old link wanted at least approximately.
   */
  it('opens the collision when the address names a run that is not published', () => {
    expect(readAddress('?run=andromeda').run.id).toBe('collision');
  });

  it('names no moment when the moment is not one', () => {
    expect(readAddress('?run=kepler&t=soon').time).toBeNull();
    expect(readAddress('?run=kepler&t=-3').time).toBeNull();
    expect(readAddress('?run=kepler').time).toBeNull();
  });

  it('reads the beginning of a run as a moment, because it is one', () => {
    expect(readAddress('?run=kepler&t=0').time).toBe(0);
  });
});

describe('writeAddress', () => {
  const cluster = GALLERY.find((run) => run.id === 'cluster');

  it('writes a run and a moment that can be read back', () => {
    if (cluster === undefined) expect.unreachable('the cluster is published');
    const written = writeAddress('', cluster, 12.5);
    expect(readAddress(written).run.id).toBe('cluster');
    expect(readAddress(written).time).toBe(12.5);
  });

  /**
   * `?renderer=` pins the backend so that the two can be compared. Choosing a
   * run must not silently put the comparison back on automatic.
   */
  it('keeps what is already in the address', () => {
    if (cluster === undefined) expect.unreachable('the cluster is published');
    const written = writeAddress('?renderer=webgl2', cluster, 1);
    expect(new URLSearchParams(written).get('renderer')).toBe('webgl2');
  });

  it('writes a moment to the digits the clock shows', () => {
    if (cluster === undefined) expect.unreachable('the cluster is published');
    expect(new URLSearchParams(writeAddress('', cluster, 1 / 3)).get('t')).toBe(
      '0.333',
    );
  });
});
