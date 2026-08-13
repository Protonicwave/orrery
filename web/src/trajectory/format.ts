/**
 * The trajectory format, read.
 *
 * A port of what `docs/formats/trajectory.md` specifies, written against the
 * document rather than against `src/sim/trajectory.cpp`, because a format
 * specification that has only ever been read by its own writer is a
 * specification nobody has checked. Where the two disagree the document is
 * wrong and should be corrected; `web/tests/trajectory/format.test.ts` reads a
 * file the C++ produced, so a disagreement fails a test rather than appearing
 * on screen as a mangled galaxy.
 *
 * Nothing here fetches anything. It is a set of functions from bytes to
 * numbers, which is what lets it be tested without a network and run inside a
 * Worker without caring how the bytes arrived.
 *
 * ## Finding the frames
 *
 * The format carries no frame count and no index, for the reason the document
 * gives: the header is written before the run has taken a step, so a count in
 * it would describe frames a killed run never wrote. It needs neither. Every
 * frame is the same length, that length follows from the particle count and the
 * flags, and the header's length follows from the same two, so the number of
 * frames is a division and the offset of frame `n` is a multiplication. A
 * reader that knows how long the file is can therefore seek to any frame
 * without having read the ones before it, which is what makes a ranged fetch
 * worth issuing.
 *
 * The division is deliberately a floor. A run cut off by a full disc or a
 * closed lid leaves a final frame that stops in the middle, and dropping it is
 * exactly what the per-frame checksums exist to make possible.
 */

/** The eight bytes a trajectory starts with. */
export const MAGIC = 'ORRERYTJ';

/** The only format version this reader knows. */
export const VERSION = 1;

/** Bit 0: the file was written in single precision, so a scalar is 4 bytes. */
export const FLAG_SINGLE_PRECISION = 1;

/** Bit 1: frames carry velocities after the positions. */
export const FLAG_VELOCITIES = 2;

/**
 * Every bit this reader understands.
 *
 * A file that sets any other bit is refused rather than read, which is what the
 * document asks of a reader: an unknown flag means a field this code does not
 * know is in the file, and reading past it would give positions assembled out
 * of the wrong bytes rather than an error.
 */
const KNOWN_FLAGS = FLAG_SINGLE_PRECISION | FLAG_VELOCITIES;

/**
 * Magic, version, flags and the particle count: everything needed to work out
 * how long the header is. Fixed, so a reader can ask for exactly this much and
 * then ask for the rest once it knows how much the rest is.
 */
export const PREFIX_LENGTH = 24;

/** Bytes a checksum occupies. */
const CHECKSUM_LENGTH = 8;

/** What a header holds, once the whole of it has been read. */
export interface TrajectoryHeader {
  readonly version: number;
  readonly flags: number;
  /** Bytes in one scalar: 4 in a single-precision file, 8 in a double one. */
  readonly scalar: 4 | 8;
  readonly count: number;
  readonly timestep: number;
  /** The masses, which do not change during a run and so are in the header. */
  readonly masses: Float64Array;
  readonly velocities: boolean;
  /** Bytes before the first frame. */
  readonly headerLength: number;
  /** Bytes in every frame, including its checksum. */
  readonly frameLength: number;
}

/** What a decoded frame says about itself. The positions go elsewhere. */
export interface FrameInfo {
  /** Counting from zero at the state the run started from. */
  readonly step: number;
  /** The simulated time, which is the step number times the timestep. */
  readonly time: number;
}

/** Somewhere to decode a frame's positions into, reused between frames. */
export interface FrameBuffers {
  readonly x: Float32Array;
  readonly y: Float32Array;
  readonly z: Float32Array;
}

/** A file this reader will not read, with the reason in the message. */
export class TrajectoryError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'TrajectoryError';
  }
}

/**
 * The 64-bit FNV-1a of a range of bytes, as a pair of 32-bit halves.
 *
 * Kept in halves rather than a BigInt because this runs over every byte of
 * every frame, and a bignum multiplication per byte would cost more than the
 * decode it is checking. The multiply below is the ordinary schoolbook one:
 * the prime is 0x100 in its high half and 0x1b3 in its low, both small enough
 * that each partial product stays well inside the range a double represents
 * exactly.
 */
export function checksum(
  bytes: Uint8Array,
  start: number,
  end: number,
  seedHigh = 0xcbf29ce4,
  seedLow = 0x84222325,
): { high: number; low: number } {
  let high = seedHigh >>> 0;
  let low = seedLow >>> 0;

  for (let index = start; index < end; index += 1) {
    low = (low ^ (bytes[index] as number)) >>> 0;

    const product = low * 0x1b3;
    const carry = Math.floor(product / 4294967296);
    const nextHigh = (high * 0x1b3 + low * 0x100 + carry) >>> 0;
    low = product >>> 0;
    high = nextHigh;
  }

  return { high, low };
}

/** Bytes before the first frame, for a file of this shape. */
export function headerLength(count: number, scalar: number): number {
  return PREFIX_LENGTH + scalar + count * scalar + CHECKSUM_LENGTH;
}

/** Bytes in one frame, for a file of this shape. */
export function frameLength(
  count: number,
  scalar: number,
  velocities: boolean,
): number {
  const components = velocities ? 6 : 3;
  return 8 + scalar + components * count * scalar + CHECKSUM_LENGTH;
}

/**
 * How many complete frames a file of this length holds.
 *
 * Floored, so a final frame that was cut in half is not counted. See the note
 * at the top of this file.
 */
export function frameCount(
  byteLength: number,
  header: Pick<TrajectoryHeader, 'headerLength' | 'frameLength'>,
): number {
  const body = byteLength - header.headerLength;
  return body <= 0 ? 0 : Math.floor(body / header.frameLength);
}

/** Where frame `index` begins. */
export function frameOffset(
  index: number,
  header: Pick<TrajectoryHeader, 'headerLength' | 'frameLength'>,
): number {
  return header.headerLength + index * header.frameLength;
}

function readScalar(view: DataView, offset: number, scalar: number): number {
  return scalar === 4 ? view.getFloat32(offset, true) : view.getFloat64(offset, true);
}

/**
 * Read the fixed prefix: enough to say how long the header is.
 *
 * Separate from `parseHeader` because the header's length depends on the
 * particle count, which is in the header. A reader over a network asks for this
 * much, works out the rest and asks for that, rather than guessing a size and
 * being wrong in one of the two directions that matter.
 */
export function parsePrefix(bytes: Uint8Array): {
  version: number;
  flags: number;
  scalar: 4 | 8;
  count: number;
  velocities: boolean;
  headerLength: number;
  frameLength: number;
} {
  if (bytes.length < PREFIX_LENGTH) {
    throw new TrajectoryError(
      `a trajectory begins with ${PREFIX_LENGTH} bytes of header and this file has ${bytes.length}`,
    );
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  let magic = '';
  for (let index = 0; index < 8; index += 1) {
    magic += String.fromCharCode(bytes[index] as number);
  }
  if (magic !== MAGIC) {
    throw new TrajectoryError(
      `this is not a trajectory: it begins ${JSON.stringify(magic)} rather than ${MAGIC}`,
    );
  }

  const version = view.getUint32(8, true);
  if (version !== VERSION) {
    throw new TrajectoryError(
      `this trajectory is version ${version} and this reader knows version ${VERSION}`,
    );
  }

  const flags = view.getUint32(12, true);
  if ((flags & ~KNOWN_FLAGS) !== 0) {
    throw new TrajectoryError(
      `this trajectory sets flag bits this reader does not know (flags ${flags})`,
    );
  }

  // The count is a 64-bit field and is read as two halves, because a particle
  // count above 2^53 is not a number JavaScript holds and a run of that size is
  // not one this client is going to draw. Anything that would not fit is
  // refused here rather than becoming a plausible-looking wrong number.
  const countLow = view.getUint32(16, true);
  const countHigh = view.getUint32(20, true);
  if (countHigh !== 0) {
    throw new TrajectoryError(
      'this trajectory holds more particles than this reader can count',
    );
  }
  const count = countLow;

  const scalar = (flags & FLAG_SINGLE_PRECISION) !== 0 ? 4 : 8;
  const velocities = (flags & FLAG_VELOCITIES) !== 0;

  return {
    version,
    flags,
    scalar,
    count,
    velocities,
    headerLength: headerLength(count, scalar),
    frameLength: frameLength(count, scalar, velocities),
  };
}

/**
 * Read the whole header, checksum included.
 *
 * `bytes` must hold at least the header. The checksum covers every byte from
 * the start of the file to the field itself, which is what the document says
 * and what makes a truncated or rewritten header fail here rather than later.
 */
export function parseHeader(bytes: Uint8Array): TrajectoryHeader {
  const prefix = parsePrefix(bytes);
  if (bytes.length < prefix.headerLength) {
    throw new TrajectoryError(
      `the header of this trajectory is ${prefix.headerLength} bytes and only ${bytes.length} arrived`,
    );
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const checksumOffset = prefix.headerLength - CHECKSUM_LENGTH;
  const computed = checksum(bytes, 0, checksumOffset);
  const storedLow = view.getUint32(checksumOffset, true);
  const storedHigh = view.getUint32(checksumOffset + 4, true);
  if (computed.low !== storedLow || computed.high !== storedHigh) {
    throw new TrajectoryError(
      'the header of this trajectory does not match its own checksum',
    );
  }

  const timestep = readScalar(view, PREFIX_LENGTH, prefix.scalar);

  const masses = new Float64Array(prefix.count);
  const massesAt = PREFIX_LENGTH + prefix.scalar;
  for (let index = 0; index < prefix.count; index += 1) {
    masses[index] = readScalar(view, massesAt + index * prefix.scalar, prefix.scalar);
  }

  return {
    version: prefix.version,
    flags: prefix.flags,
    scalar: prefix.scalar,
    count: prefix.count,
    timestep,
    masses,
    velocities: prefix.velocities,
    headerLength: prefix.headerLength,
    frameLength: prefix.frameLength,
  };
}

/**
 * Decode one frame's positions into buffers the caller already owns.
 *
 * The positions are read a component at a time rather than viewed in place.
 * Two reasons, and either alone would be enough: a double-precision file holds
 * values the renderer cannot use without converting them, and a ranged fetch
 * returns a buffer that begins wherever the range did, so a typed-array view
 * over it would fail on an alignment the network chose.
 *
 * Returns the step and time. Throws if the frame does not match its checksum,
 * which for the last frame of an interrupted run is the correct answer.
 */
export function decodeFrame(
  bytes: Uint8Array,
  offset: number,
  header: TrajectoryHeader,
  into: FrameBuffers,
): FrameInfo {
  if (offset + header.frameLength > bytes.length) {
    throw new TrajectoryError('this frame is not all here');
  }
  if (
    into.x.length < header.count ||
    into.y.length < header.count ||
    into.z.length < header.count
  ) {
    throw new TrajectoryError(
      'the buffers offered are smaller than the frame they are to receive',
    );
  }

  const checksumOffset = offset + header.frameLength - CHECKSUM_LENGTH;
  const computed = checksum(bytes, offset, checksumOffset);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const storedLow = view.getUint32(checksumOffset, true);
  const storedHigh = view.getUint32(checksumOffset + 4, true);
  if (computed.low !== storedLow || computed.high !== storedHigh) {
    throw new TrajectoryError('this frame does not match its own checksum');
  }

  const stepLow = view.getUint32(offset, true);
  const stepHigh = view.getUint32(offset + 4, true);
  const step = stepHigh * 4294967296 + stepLow;
  const time = readScalar(view, offset + 8, header.scalar);

  const { scalar, count } = header;
  const block = count * scalar;
  let at = offset + 8 + scalar;

  if (scalar === 4) {
    for (let index = 0; index < count; index += 1) {
      into.x[index] = view.getFloat32(at + index * 4, true);
    }
    at += block;
    for (let index = 0; index < count; index += 1) {
      into.y[index] = view.getFloat32(at + index * 4, true);
    }
    at += block;
    for (let index = 0; index < count; index += 1) {
      into.z[index] = view.getFloat32(at + index * 4, true);
    }
  } else {
    for (let index = 0; index < count; index += 1) {
      into.x[index] = view.getFloat64(at + index * 8, true);
    }
    at += block;
    for (let index = 0; index < count; index += 1) {
      into.y[index] = view.getFloat64(at + index * 8, true);
    }
    at += block;
    for (let index = 0; index < count; index += 1) {
      into.z[index] = view.getFloat64(at + index * 8, true);
    }
  }

  return { step, time };
}
