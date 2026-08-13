/**
 * What the trajectory Worker and the page say to each other.
 *
 * A module of types with no code in it, imported by both sides, so that the
 * two cannot drift: a message the Worker posts and a message the page expects
 * are the same declaration read twice rather than two declarations kept in
 * step by hand. ADR-0047.
 */

/** Open this trajectory and start walking it. */
export interface OpenMessage {
  readonly kind: 'open';
  readonly url: string;
}

export type ToWorker = OpenMessage;

/** The header, read. Sent once, before any frame. */
export interface OpenedMessage {
  readonly kind: 'opened';
  readonly count: number;
  readonly timestep: number;
  readonly frames: number;
  /** Bytes in a scalar, which is the precision the run was written in. */
  readonly scalar: number;
  readonly velocities: boolean;
  readonly byteLength: number;
}

/**
 * One frame. The three component arrays are transferred rather than copied,
 * so this message costs the same whatever the particle count.
 */
export interface FrameMessage {
  readonly kind: 'frame';
  readonly index: number;
  readonly step: number;
  readonly time: number;
  readonly x: Float32Array;
  readonly y: Float32Array;
  readonly z: Float32Array;
}

/** Every frame has been read. */
export interface CompleteMessage {
  readonly kind: 'complete';
}

/**
 * Something went wrong, in a sentence fit to put on the plate.
 *
 * A failure is a message rather than a rejected promise because the Worker
 * keeps going after one: a frame that fails its checksum ends the walk, and
 * the frames already delivered are still a run worth playing.
 */
export interface FailedMessage {
  readonly kind: 'failed';
  readonly message: string;
  /** Frames delivered before it failed. */
  readonly delivered: number;
}

export type FromWorker = OpenedMessage | FrameMessage | CompleteMessage | FailedMessage;
