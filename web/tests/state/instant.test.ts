import { describe, expect, it } from 'vitest';
import { InstantSource } from '../../src/state/instant';

describe('InstantSource', () => {
  it('tells a listener the instant as soon as it subscribes', () => {
    const instants = new InstantSource();
    instants.publish(2.5, 500);

    const seen: number[] = [];
    instants.subscribe((time) => seen.push(time));
    expect(seen).toEqual([2.5]);
  });

  it('tells every listener what was published', () => {
    const instants = new InstantSource();
    const times: number[] = [];
    const steps: number[] = [];
    instants.subscribe((time) => times.push(time));
    instants.subscribe((_, step) => steps.push(step));

    instants.publish(1, 200);
    expect(times).toEqual([0, 1]);
    expect(steps).toEqual([0, 200]);
    expect(instants.time).toBe(1);
    expect(instants.step).toBe(200);
  });

  it('stops telling a listener that has been released', () => {
    const instants = new InstantSource();
    const seen: number[] = [];
    const release = instants.subscribe((time) => seen.push(time));
    release();

    instants.publish(3, 600);
    expect(seen).toEqual([0]);
  });

  /**
   * The plots are released when the console hides them, which happens while the
   * run is playing and therefore while a notification may be in flight.
   */
  it('survives a listener releasing itself during a notification', () => {
    const instants = new InstantSource();
    const seen: number[] = [];
    let release = () => {};
    release = instants.subscribe(() => {
      release();
    });
    instants.subscribe((time) => seen.push(time));

    expect(() => instants.publish(4, 800)).not.toThrow();
    expect(seen).toEqual([0, 4]);
  });
});
