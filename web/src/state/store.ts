/**
 * The client's state, in two kinds that never mix.
 *
 * Chrome state is what a person changes: which tone curve is selected, whether
 * trails are drawn, where the transport is. It changes when a control is
 * operated, which is hundreds of times an hour, and React may render it.
 *
 * Render-loop state is what the run changes: the model time, the step, the
 * frame rate. It changes sixty times a second, and a value at sixty hertz must
 * never touch a virtual DOM. It lives in the mutable record below, is read
 * directly by the render loop and written to the document by whatever displays
 * it, and it notifies nobody.
 *
 * Keeping the two apart is the reason there is no state library here. A store
 * general enough to hold both would have to be told, at every write, which of
 * the two it was doing.
 */

import type { ToneCurve } from '../render/renderer';

/**
 * Which tone-mapping curve the renderer applies. Defined by the renderer, and
 * carried through here so that the console reads one definition of it.
 */
export type { ToneCurve };

export type Listener<T> = (state: T) => void;

export interface Store<T> {
  /** The current state. Stable between writes, so it can be compared by identity. */
  get(): T;
  /** Merge a patch, or apply a function to the current state. */
  set(patch: Partial<T> | ((state: T) => Partial<T>)): void;
  /** Subscribe to writes. Returns the function that unsubscribes. */
  subscribe(listener: Listener<T>): () => void;
}

/**
 * A typed store with subscriptions, and nothing else.
 *
 * A write that changes no value notifies nobody: the interface has controls
 * that are dragged, and a slider held still at 1.35 should not re-render
 * anything sixty times a second on the way to staying where it is.
 */
export function createStore<T extends object>(initial: T): Store<T> {
  let state = initial;
  const listeners = new Set<Listener<T>>();

  return {
    get: () => state,

    set(patch) {
      const change = typeof patch === 'function' ? patch(state) : patch;
      let changed = false;
      for (const key of Object.keys(change) as (keyof T)[]) {
        if (!Object.is(state[key], change[key])) {
          changed = true;
          break;
        }
      }
      if (!changed) return;

      state = { ...state, ...change };
      // A copy, so that a listener unsubscribing during the notification does
      // not change the set being walked.
      for (const listener of [...listeners]) listener(state);
    },

    subscribe(listener) {
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },
  };
}

/** What the plate draws over the particles. */
export type Overlay = 'none' | 'octree' | 'density';

/** Which integrator a new run would be given. */
export type Integrator = 'velocity-verlet' | 'yoshida4' | 'rk4';

/** Everything a person can change without a new run being needed. */
export interface ChromeState {
  /** Stops in the exposure, applied as a gain before tone mapping. */
  readonly exposure: number;
  /** Sprite radius, as a multiple of the renderer's default. */
  readonly spriteRadius: number;
  readonly toneCurve: ToneCurve;
  readonly trails: boolean;
  readonly labFrame: boolean;
  readonly bulgeOnly: boolean;
  readonly overlay: Overlay;
  /** How far the rotating frame turns with the pair, from zero to one. */
  readonly rotatingFrame: number;
  /** Steps of orbit history drawn behind each particle. */
  readonly orbitTrace: number;
  readonly diagnostics: boolean;
  readonly radialProfile: boolean;
  readonly boundUnbound: boolean;
  readonly playing: boolean;
  /** What a new run would be asked for. Nothing acts on these until Phase 8. */
  readonly requestedCount: number;
  readonly requestedSoftening: number;
  readonly requestedIntegrator: Integrator;
}

/**
 * The smallest count the solver tier's slider can express.
 *
 * Used for a run whose configuration states no count, which is the two-body
 * problem: it names two masses, because the count is a property of that
 * scenario rather than a setting of it. A slider asking what a new run should
 * be given has to open somewhere, and the smallest it offers is the honest
 * answer when the run in front of it cannot suggest one.
 */
const SMALLEST_REQUEST = 5_000;

/**
 * The defaults are the run's own settings, so the interface opens showing the
 * configuration it is displaying rather than a set of numbers chosen to look
 * reasonable. The two exposure and sprite values are the viewer's defaults
 * from docs/visualisation.md.
 */
export function createChromeState(defaults: {
  count: number | undefined;
  softening: number;
  integrator: Integrator;
}): Store<ChromeState> {
  return createStore<ChromeState>({
    exposure: 1.6,
    spriteRadius: 1.35,
    toneCurve: 'reinhard',
    trails: false,
    labFrame: false,
    bulgeOnly: false,
    overlay: 'none',
    rotatingFrame: 0,
    orbitTrace: 0,
    diagnostics: true,
    radialProfile: false,
    boundUnbound: false,
    playing: false,
    requestedCount: Math.max(defaults.count ?? 0, SMALLEST_REQUEST),
    requestedSoftening: defaults.softening,
    requestedIntegrator: defaults.integrator,
  });
}

/**
 * The values that change every frame.
 *
 * Deliberately a mutable record with no subscription mechanism. The render
 * loop writes it and reads it, and whatever displays a field writes that field
 * to the document itself. Adding a listener here would put a callback on the
 * hot path, and the budget for that path is no allocation at all.
 */
export interface FrameState {
  /** Model time, in the run's units. */
  time: number;
  /** The step that time corresponds to. */
  step: number;
  /** Frames a second, smoothed. */
  framesPerSecond: number;
  /** Milliseconds the last solver step took, when one is running. */
  stepTime: number;
}

export function createFrameState(): FrameState {
  return { time: 0, step: 0, framesPerSecond: 0, stepTime: 0 };
}
