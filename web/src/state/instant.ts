/**
 * Which instant of the run everything on screen is reading.
 *
 * A third kind of state, between the two `state/store.ts` describes. It is not
 * chrome state, because nobody sets it: the render loop does, ten times a
 * second. It is not render-loop state either, because several parts of the
 * interface have to follow it, and the whole point of `FrameState` is that it
 * notifies nobody.
 *
 * So it is a broadcast, and what it broadcasts to writes the value into the
 * document itself rather than rendering it. The transport's clock, the four
 * sparkline cursors and the four values direct-labelled above them are all the
 * same instant, and going through React for them would be nine components
 * re-rendering ten times a second to move a cursor by a pixel.
 *
 * Ten times a second and not sixty. That rate is set in `render/instrument.ts`
 * for the reason given there: it is the rate a person can read a clock at.
 */

export type InstantListener = (time: number, step: number) => void;

export class InstantSource {
  /** The last instant published, so a listener can catch up on subscribing. */
  time = 0;
  step = 0;

  private readonly listeners = new Set<InstantListener>();

  publish(time: number, step: number): void {
    this.time = time;
    this.step = step;
    // A copy, so that a listener unsubscribing during the notification does not
    // change the set being walked.
    for (const listener of [...this.listeners]) listener(time, step);
  }

  /** Subscribe, and be told the current instant at once. Returns the release. */
  subscribe(listener: InstantListener): () => void {
    this.listeners.add(listener);
    listener(this.time, this.step);
    return () => {
      this.listeners.delete(listener);
    };
  }
}
