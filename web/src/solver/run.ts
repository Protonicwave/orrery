/**
 * The page's half of the browser solver.
 *
 * Starts the Worker, keeps the frames it sends back, and says what the run has
 * achieved. Nothing here integrates anything, and nothing here decides what the
 * configuration should be: the caller passes a plan, made once the module has
 * said what size of run it will accept, so that the note beside the control and
 * the document handed across cannot disagree. A published run plans itself with
 * `configure.ts` and a design in the editor plans itself from the drawing, and
 * this treats the two the same way because from here they are the same thing.
 *
 * It is a `FrameSource`, which is the interface the published trajectory also
 * satisfies, so the render loop draws a browser run and a published run through
 * the same path.
 */

import type {
  FramePositions,
  FrameSource,
  SourceFacts,
  TrajectoryStatus,
} from '../trajectory/client';
import type { BrowserRun } from './configure';
import { MEASUREMENT } from './module';
import type { FromWorker, ToWorker } from './protocol';

/**
 * What the run turned out to be, as opposed to what it was asked to be.
 *
 * Every field is measured or reported by the module rather than assumed by the
 * page. It is on screen the whole time a browser run is going, because a
 * picture drawn by a scalar kernel on one thread must never be mistaken for one
 * of the figures in `docs/performance/`, and the only way to prevent that is to
 * state what actually produced it.
 */
export interface Achieved {
  readonly count: number;
  readonly solver: string;
  readonly kernel: string;
  /** What the module changed about the configuration, or an empty string. */
  readonly report: string;
  /** Milliseconds a step is taking, smoothed over the frames so far. */
  readonly stepMilliseconds: number;
  /** The relative energy error at the last measurement, or null before one. */
  readonly energyDrift: number | null;
  readonly virialRatio: number | null;
}

/** How quickly the step-time readout follows the truth. */
const RATE_SMOOTHING = 0.2;

export class LiveRun implements FrameSource {
  status: TrajectoryStatus = 'opening';
  facts: SourceFacts | null = null;
  message = '';

  achieved: Achieved = {
    count: 0,
    solver: '',
    kernel: '',
    report: '',
    stepMilliseconds: 0,
    energyDrift: null,
    virialRatio: null,
  };

  /**
   * What was asked for, once the module has said what it will accept.
   *
   * Null until then. It is what the note beside the control states, and it is
   * derived from the module's own limit rather than from a copy of it.
   */
  plan: BrowserRun | null = null;

  /** The largest particle count the module will run, once it has said. */
  particleLimit = 0;

  private readonly cache: (FramePositions | undefined)[] = [];
  private readonly listeners = new Set<() => void>();
  private worker: Worker | null = null;
  private ready = 0;
  private planner: ((particleLimit: number) => BrowserRun) | null = null;

  get available(): number {
    return this.ready;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  frame(index: number): FramePositions | undefined {
    return this.cache[index];
  }

  /**
   * Which frame holds a moment in model time.
   *
   * The spacing is the stride times the timestep, both of which the Worker
   * reported before the first frame, so unlike the trajectory reader this can
   * answer before two frames have arrived.
   */
  indexAt(time: number): number {
    const first = this.cache[0];
    if (this.facts === null || first === undefined) return -1;

    const interval = this.facts.timestep * this.stride;
    if (!(interval > 0)) return -1;
    return Math.round((time - first.time) / interval);
  }

  frameAt(time: number): FramePositions | undefined {
    const index = this.indexAt(time);
    return index < 0 ? undefined : this.cache[index];
  }

  private stride = 1;

  /**
   * Start integrating whatever `plan` asks for.
   *
   * Safe to call once; a second call is ignored. The plan is a function rather
   * than a configuration because what a browser run should be depends on the
   * module's own particle limit, and that is not known until the module has
   * loaded, which is why nothing is sent to it here beyond the request to load.
   */
  start(plan: (particleLimit: number) => BrowserRun): void {
    if (this.worker !== null) return;
    this.planner = plan;

    // Integrating happens off the main thread or it does not happen, for the
    // same reason decoding does. A browser with no Worker says so rather than
    // being given a step on the thread that also draws.
    if (typeof Worker === 'undefined') {
      this.status = 'failed';
      this.message =
        'This browser has no Worker, and the solver runs off the main thread.';
      this.notify();
      return;
    }

    this.worker = new Worker(new URL('./worker.ts', import.meta.url), {
      type: 'module',
      name: 'solver',
    });
    this.worker.onmessage = (event: MessageEvent<FromWorker>) => {
      this.receive(event.data);
    };
    this.worker.onerror = (event) => {
      this.status = 'failed';
      this.message = event.message || 'the solver stopped';
      this.notify();
    };

    // The site's base, absolute, because the Worker cannot work out where the
    // module is from its own location: it has been bundled into a chunk that
    // does not sit beside it.
    const message: ToWorker = {
      kind: 'load',
      base: new URL(import.meta.env.BASE_URL, window.location.href).href,
    };
    this.worker.postMessage(message);
  }

  /** Stop integrating and release the Worker. */
  stop(): void {
    const stopping: ToWorker = { kind: 'stop' };
    this.worker?.postMessage(stopping);
    this.worker?.terminate();
    this.worker = null;
  }

  private receive(message: FromWorker): void {
    switch (message.kind) {
      case 'ready': {
        this.particleLimit = message.particleLimit;
        const planner = this.planner;
        if (planner === null || this.worker === null) return;
        this.plan = planner(message.particleLimit);
        const start: ToWorker = {
          kind: 'start',
          configuration: this.plan.text,
          steps: this.plan.steps,
          stride: this.plan.stride,
        };
        this.worker.postMessage(start);
        this.notify();
        break;
      }

      case 'started':
        this.stride = message.stride;
        this.facts = {
          count: message.count,
          timestep: message.timestep,
          frames: message.frames,
          masses: message.masses,
        };
        this.achieved = {
          ...this.achieved,
          count: message.count,
          solver: message.solver,
          kernel: message.kernel,
          report: message.report,
        };
        this.cache.length = message.frames;
        this.status = 'streaming';
        this.notify();
        break;

      case 'frame': {
        this.cache[message.index] = message;
        while (this.cache[this.ready] !== undefined) this.ready += 1;

        // The first frame is the state the run started from and took no step to
        // reach, so it contributes no step time.
        if (message.stepMilliseconds > 0) {
          const held = this.achieved.stepMilliseconds;
          this.achieved = {
            ...this.achieved,
            stepMilliseconds:
              held === 0
                ? message.stepMilliseconds
                : held + (message.stepMilliseconds - held) * RATE_SMOOTHING,
          };
        }

        // Every frame, unlike the trajectory reader's hundredth. A browser run
        // records a few hundred frames over a minute rather than four hundred
        // over a few seconds, and the readouts beside the plate are the reason
        // anyone is watching it.
        this.notify();
        break;
      }

      case 'measured':
        this.achieved = {
          ...this.achieved,
          energyDrift: message.values[MEASUREMENT.energyDrift] as number,
          virialRatio: message.values[MEASUREMENT.virialRatio] as number,
        };
        this.notify();
        break;

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
