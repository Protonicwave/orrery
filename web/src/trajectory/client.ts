/**
 * The page's half of the trajectory reader.
 *
 * Starts the Worker, keeps the frames it sends back, and says how much of the
 * run has arrived. Nothing here decodes anything.
 *
 * The frames are all kept. The published run is four hundred frames of eight
 * thousand particles, which is thirty-nine megabytes of Float32Array, and
 * holding it means that playing the run a second time costs no network at all
 * and that scrubbing to any instant is a lookup. Dropping frames to save that
 * memory would trade a resource the browser has for one it does not: a
 * connection that has to be asked again every time the transport moves.
 */

import type { FromWorker, ToWorker } from './protocol';

/**
 * What anything the instrument can draw has to be able to say about itself.
 *
 * A published trajectory knows more than this, and says so below. A run being
 * integrated in the browser knows only this, and that is the whole of what the
 * plate, the transport and the render loop read.
 */
export interface SourceFacts {
  readonly count: number;
  readonly timestep: number;
  readonly frames: number;
  /** The masses, in particle order. Constant for the length of the run. */
  readonly masses: Float64Array;
}

/** What the header said, once it has been read. */
export interface TrajectoryFacts extends SourceFacts {
  readonly scalar: number;
  readonly velocities: boolean;
  readonly byteLength: number;
}

export type TrajectoryStatus = 'opening' | 'streaming' | 'complete' | 'failed';

/** One frame's positions, as the render loop reads them. */
export interface FramePositions {
  readonly step: number;
  readonly time: number;
  readonly x: Float32Array;
  readonly y: Float32Array;
  readonly z: Float32Array;
}

/**
 * How often the page is told that more of the run has arrived.
 *
 * Every hundredth of the run rather than every frame. A frame message arrives
 * four hundred times and a re-render of the chrome for each of them would be
 * four hundred renders to move a progress readout that has a hundred distinct
 * values.
 */
const NOTIFY_EVERY = 0.01;

/**
 * A growing sequence of frames the instrument can play.
 *
 * Two things are one: a published trajectory arriving over a connection, and a
 * run being integrated in a Worker. Both hand the render loop frames in order,
 * both grow while they are being watched, and both can fail partway through and
 * still be worth looking at. The loop is written against this rather than
 * against either of them, which is what lets the plate draw a browser run
 * without knowing that is what it is doing.
 */
export interface FrameSource {
  readonly status: TrajectoryStatus;
  readonly facts: SourceFacts | null;
  /** Empty unless it failed, and then a sentence saying how. */
  readonly message: string;
  /** Frames that can be drawn, counting from the start and without a gap. */
  readonly available: number;
  subscribe(listener: () => void): () => void;
  frame(index: number): FramePositions | undefined;
  /** Which frame holds a moment in model time, or −1 if that is not yet known. */
  indexAt(time: number): number;
}

export class Trajectory implements FrameSource {
  status: TrajectoryStatus = 'opening';
  facts: TrajectoryFacts | null = null;
  /** Empty until it fails, and then a sentence saying how. */
  message = '';

  private readonly cache: (FramePositions | undefined)[] = [];
  private readonly listeners = new Set<() => void>();
  private worker: Worker | null = null;
  private ready = 0;
  private notifiedAt = 0;

  /**
   * Frames that can be drawn, counting from the start.
   *
   * Contiguous rather than a count of what has arrived: the Worker walks the
   * file in order, and a transport that could jump to a frame with a gap
   * before it would be reading a run that has not been read.
   */
  get available(): number {
    return this.ready;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** The frame at `index`, or undefined if it has not arrived. */
  frame(index: number): FramePositions | undefined {
    return this.cache[index];
  }

  /**
   * Which frame holds a moment in model time, whether or not it has arrived.
   *
   * The interval between frames is taken from the first two frames rather than
   * from the configuration's stride, because the trajectory is what is being
   * played and a configuration edited since the run would be a second answer to
   * the same question. Frames are written at a fixed stride, so two of them are
   * enough to know where all of them are.
   *
   * Answers −1 while fewer than two frames have arrived, which is the only
   * state in which the spacing is not yet known.
   */
  indexAt(time: number): number {
    const first = this.cache[0];
    const second = this.cache[1];
    if (first === undefined || second === undefined) return -1;

    const interval = second.time - first.time;
    if (!(interval > 0)) return -1;
    return Math.round((time - first.time) / interval);
  }

  /** The frame holding a moment, if it has arrived. */
  frameAt(time: number): FramePositions | undefined {
    const index = this.indexAt(time);
    return index < 0 ? undefined : this.cache[index];
  }

  /** Start reading. Safe to call once; a second call is ignored. */
  open(url: string): void {
    if (this.worker !== null) return;

    // Decoding happens off the main thread or it does not happen. A browser
    // with no Worker would have to be given the decode on the thread that also
    // runs the render loop, which is the arrangement ADR-0047 exists to
    // prevent, so this says so instead.
    if (typeof Worker === 'undefined') {
      this.status = 'failed';
      this.message =
        'This browser has no Worker, and the trajectory is decoded off the main thread.';
      this.notify();
      return;
    }

    this.worker = new Worker(new URL('./worker.ts', import.meta.url), {
      type: 'module',
      name: 'trajectory',
    });
    this.worker.onmessage = (event: MessageEvent<FromWorker>) => {
      this.receive(event.data);
    };
    this.worker.onerror = (event) => {
      this.status = 'failed';
      this.message = event.message || 'the trajectory reader stopped';
      this.notify();
    };

    const message: ToWorker = { kind: 'open', url };
    this.worker.postMessage(message);
  }

  /** Stop reading and release the Worker. */
  close(): void {
    this.worker?.terminate();
    this.worker = null;
  }

  private receive(message: FromWorker): void {
    switch (message.kind) {
      case 'opened':
        this.facts = message;
        this.status = 'streaming';
        this.cache.length = message.frames;
        this.notify();
        break;

      case 'frame': {
        this.cache[message.index] = message;
        while (this.cache[this.ready] !== undefined) this.ready += 1;

        const frames = this.facts?.frames ?? 0;
        const share = frames === 0 ? 1 : this.ready / frames;
        if (share - this.notifiedAt >= NOTIFY_EVERY || this.ready === frames) {
          this.notifiedAt = share;
          this.notify();
        }
        break;
      }

      case 'complete':
        this.status = 'complete';
        this.notify();
        break;

      case 'failed':
        this.status = 'failed';
        this.message = message.message;
        this.notify();
        break;
    }
  }

  private notify(): void {
    for (const listener of [...this.listeners]) listener();
  }
}
