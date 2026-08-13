/**
 * Reading a trajectory a piece at a time.
 *
 * The published gallery run is tens of megabytes and the first frame of it is
 * a hundred kilobytes, so a reader that waits for the file has an instrument
 * that shows nothing for most of a minute on a connection that is not the one
 * it was built on. This reads the header, works out where the frames are, and
 * then walks them in ranged requests of several frames at a time, handing each
 * frame on as soon as it has been decoded and checked.
 *
 * Requests cover several frames rather than one because a request has a cost
 * that has nothing to do with how many bytes it carries. A frame of eight
 * thousand particles is ninety-six kilobytes, and asking for one at a time
 * would spend more of the download on round trips than on positions.
 */

import {
  decodeFrame,
  type FrameInfo,
  frameCount,
  frameOffset,
  PREFIX_LENGTH,
  parseHeader,
  parsePrefix,
  TrajectoryError,
  type TrajectoryHeader,
} from './format';
import type { ByteSource } from './source';

/**
 * How much of the file the first request asks for.
 *
 * The header is the particle count times a scalar plus a little, so
 * sixty-four kilobytes covers every run up to eight thousand particles in
 * single precision in one request. A larger run costs a second request and no
 * more than that, because the first one is what says how large the header is.
 */
const FIRST_READ = 64 * 1024;

/**
 * The size a frame request aims for.
 *
 * Half a megabyte: large enough that the round trip is a small part of what the
 * request costs, small enough that the first frames are on screen while the
 * rest of the run is still arriving. Rounded down to a whole number of frames,
 * and never less than one.
 */
const CHUNK_TARGET = 512 * 1024;

/** One frame, decoded, with buffers of its own. */
export interface DecodedFrame extends FrameInfo {
  readonly index: number;
  readonly x: Float32Array;
  readonly y: Float32Array;
  readonly z: Float32Array;
}

export class TrajectoryReader {
  readonly header: TrajectoryHeader;
  readonly frames: number;
  readonly byteLength: number;

  private readonly source: ByteSource;

  private constructor(
    source: ByteSource,
    header: TrajectoryHeader,
    byteLength: number,
  ) {
    this.source = source;
    this.header = header;
    this.byteLength = byteLength;
    this.frames = frameCount(byteLength, header);
  }

  /** Read the header and work out how many frames the file holds. */
  static async open(source: ByteSource): Promise<TrajectoryReader> {
    const first = await source.read(0, FIRST_READ);
    if (!Number.isFinite(first.totalLength)) {
      throw new TrajectoryError(
        `${source.name} did not say how long it is, and the frames of a trajectory are found by dividing its length`,
      );
    }

    const prefix = parsePrefix(first.bytes.subarray(0, PREFIX_LENGTH));
    const bytes =
      first.bytes.length >= prefix.headerLength
        ? first.bytes
        : (await source.read(0, prefix.headerLength)).bytes;

    return new TrajectoryReader(source, parseHeader(bytes), first.totalLength);
  }

  /** How many frames one request covers. */
  get framesPerRequest(): number {
    return Math.max(1, Math.floor(CHUNK_TARGET / this.header.frameLength));
  }

  /**
   * Walk the frames from `from` to the end, yielding each as it is decoded.
   *
   * Each frame is yielded with buffers of its own rather than into buffers the
   * caller lends, because the Worker this runs in transfers them away and a
   * transferred buffer is detached. Allocation here is once per frame during a
   * download, which is not a hot path; the render loop that eventually draws
   * them allocates nothing at all.
   */
  async *walk(from = 0): AsyncGenerator<DecodedFrame> {
    const { header } = this;
    const perRequest = this.framesPerRequest;

    for (let first = from; first < this.frames; first += perRequest) {
      const last = Math.min(first + perRequest, this.frames);
      const start = frameOffset(first, header);
      const end = frameOffset(last, header);
      const chunk = await this.source.read(start, end);

      if (chunk.bytes.length < end - start) {
        throw new TrajectoryError(
          `${this.source.name} returned ${chunk.bytes.length} bytes where ${end - start} were asked for`,
        );
      }

      for (let index = first; index < last; index += 1) {
        const x = new Float32Array(header.count);
        const y = new Float32Array(header.count);
        const z = new Float32Array(header.count);
        const info = decodeFrame(
          chunk.bytes,
          (index - first) * header.frameLength,
          header,
          { x, y, z },
        );
        yield { index, step: info.step, time: info.time, x, y, z };
      }
    }
  }
}
