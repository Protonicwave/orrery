/**
 * What the solver Worker and the page say to each other.
 *
 * A module of types with no code in it, imported by both sides, so that the two
 * cannot drift: a message the Worker posts and a message the page expects are
 * the same declaration read twice rather than two declarations kept in step by
 * hand. It is the trajectory reader's arrangement (ADR-0047) applied to the
 * other thing this client must not do on the main thread, which is integrate.
 */

/**
 * Load the module and say what it will and will not do.
 *
 * A separate message from `start` because the page cannot write the
 * configuration until it knows the module's particle limit, and that limit has
 * one home, which is the C++ that enforces it. Asking is a round trip; keeping
 * a second copy of the number in the client would be a number that one day
 * differs from the one being enforced.
 */
export interface LoadMessage {
  readonly kind: 'load';
  /**
   * Where the site's static assets are, as an absolute URL.
   *
   * Sent rather than worked out in the Worker, because a bundled Worker's own
   * location is the chunk it was compiled into and the module is not beside
   * that chunk. The page knows where the site is and the Worker does not.
   */
  readonly base: string;
}

/**
 * Start a run from the text of an `.orrery` configuration file.
 *
 * Always follows a `load`, which is what tells the Worker where the module is
 * and what tells the page what size of run the module will accept.
 */
export interface StartMessage {
  readonly kind: 'start';
  readonly configuration: string;
  /** Steps between recorded frames. */
  readonly stride: number;
  /** Steps to take altogether. */
  readonly steps: number;
}

/** Stop the run and release the module. */
export interface StopMessage {
  readonly kind: 'stop';
}

export type ToWorker = LoadMessage | StartMessage | StopMessage;

/** The module is instantiated, and these are its bounds. */
export interface ReadyMessage {
  readonly kind: 'ready';
  /** The largest particle count the module will run. */
  readonly particleLimit: number;
  /** The most steps one call into the module may take. */
  readonly stepLimit: number;
  /** Bytes in a scalar: 8 for the double-precision build, 4 for single. */
  readonly scalarSize: number;
}

/** The run has been assembled. Sent once, before any frame. */
export interface StartedMessage {
  readonly kind: 'started';
  /** Particles the configuration actually produced, which may not be what it asked for. */
  readonly count: number;
  readonly timestep: number;
  readonly steps: number;
  readonly stride: number;
  readonly frames: number;
  /** `direct` or `barnes-hut`. */
  readonly solver: string;
  /** `scalar`, which is the only kernel compiled for this target. */
  readonly kernel: string;
  /** What the module changed about the configuration, or an empty string. */
  readonly report: string;
  /** The masses, in particle order, transferred rather than copied. */
  readonly masses: Float64Array;
}

/**
 * One recorded frame. The three component arrays are transferred rather than
 * copied, so this message costs the same whatever the particle count.
 */
export interface FrameMessage {
  readonly kind: 'frame';
  readonly index: number;
  readonly step: number;
  readonly time: number;
  readonly x: Float32Array;
  readonly y: Float32Array;
  readonly z: Float32Array;
  /** The mean step time over the steps since the last frame, in milliseconds. */
  readonly stepMilliseconds: number;
}

/**
 * The conserved quantities at one instant.
 *
 * Eleven numbers in the order `wasm/orrery_wasm.h` fixes, which is the order a
 * diagnostics file carries its columns in.
 */
export interface MeasuredMessage {
  readonly kind: 'measured';
  readonly step: number;
  readonly time: number;
  readonly values: Float64Array;
}

/** The run reached its last step. */
export interface CompleteMessage {
  readonly kind: 'complete';
}

/**
 * Something went wrong, in a sentence fit to put on the plate.
 *
 * A failure is a message rather than a rejected promise for the reason the
 * trajectory reader gives: the frames already delivered are still a run worth
 * looking at, and a module that will not load at all is a thing to say plainly
 * rather than an exception to swallow.
 */
export interface FailedMessage {
  readonly kind: 'failed';
  readonly message: string;
  /** Frames delivered before it failed. */
  readonly delivered: number;
}

export type FromWorker =
  | ReadyMessage
  | StartedMessage
  | FrameMessage
  | MeasuredMessage
  | CompleteMessage
  | FailedMessage;
