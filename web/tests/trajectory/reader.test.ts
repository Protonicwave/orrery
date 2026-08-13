import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { TrajectoryReader } from '../../src/trajectory/reader';
import { type ByteSource, bufferSource } from '../../src/trajectory/source';

/**
 * The reader, over a source that is not a network.
 *
 * The point of the split between `source.ts` and `reader.ts` is that this file
 * exists: the ranged walk, the chunking and the frame arithmetic are all
 * exercised here against a file the C++ wrote, with no server started and no
 * fetch mocked.
 */

const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), '..', 'fixtures');
const file = readFileSync(join(FIXTURES, 'kepler-double.otj'));
const buffer = file.buffer.slice(
  file.byteOffset,
  file.byteOffset + file.byteLength,
) as ArrayBuffer;

/** A source that records what was asked for, so the walk can be inspected. */
function recording(): { source: ByteSource; ranges: [number, number][] } {
  const inner = bufferSource(buffer, 'the fixture');
  const ranges: [number, number][] = [];
  return {
    ranges,
    source: {
      name: inner.name,
      read(start, end) {
        ranges.push([start, end]);
        return inner.read(start, end);
      },
    },
  };
}

describe('opening a trajectory', () => {
  it('reads the header and works out how many frames there are', async () => {
    const reader = await TrajectoryReader.open(bufferSource(buffer));
    expect(reader.header.count).toBe(2);
    expect(reader.frames).toBe(11);
    expect(reader.byteLength).toBe(848);
  });

  it('walks every frame in order, with its step and its time', async () => {
    const reader = await TrajectoryReader.open(bufferSource(buffer));
    const steps: number[] = [];
    for await (const frame of reader.walk()) {
      expect(frame.index).toBe(steps.length);
      expect(frame.x.length).toBe(2);
      steps.push(frame.step);
    }
    expect(steps).toEqual([0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200]);
  });

  it('starts where it is asked to', async () => {
    const reader = await TrajectoryReader.open(bufferSource(buffer));
    const indices: number[] = [];
    for await (const frame of reader.walk(8)) indices.push(frame.index);
    expect(indices).toEqual([8, 9, 10]);
  });

  it('asks for several frames at a time, on frame boundaries', async () => {
    const { source, ranges } = recording();
    const reader = await TrajectoryReader.open(source);
    for await (const _frame of reader.walk()) {
      // Walked for the requests it makes, not for what it decodes.
    }

    // The first request is the header's, and every one after it begins and ends
    // where a frame does. A request that began part way through a frame would
    // still decode, because the reader is told the offset, and it would fetch
    // bytes it could not use.
    const { headerLength, frameLength } = reader.header;
    for (const [start, end] of ranges.slice(1)) {
      expect((start - headerLength) % frameLength).toBe(0);
      expect((end - headerLength) % frameLength).toBe(0);
    }

    // Eleven frames of seventy-two bytes fit in one request of the size the
    // reader aims for, so this file is one header request and one body request.
    expect(ranges.length).toBe(2);
  });

  it('says so rather than guessing when the length is not known', async () => {
    const source: ByteSource = {
      name: 'a server that will not say',
      read(start, end) {
        return bufferSource(buffer)
          .read(start, end)
          .then((chunk) => ({ bytes: chunk.bytes, totalLength: Number.NaN }));
      },
    };

    await expect(TrajectoryReader.open(source)).rejects.toThrow(
      /did not say how long it is/,
    );
  });

  it('stops rather than inventing a frame that was not all sent', async () => {
    const short: ByteSource = {
      name: 'a source that runs out',
      async read(start, end) {
        const chunk = await bufferSource(buffer).read(start, end);
        // Everything up to the first frame arrives; the body is cut short.
        return start === 0
          ? chunk
          : { bytes: chunk.bytes.subarray(0, 10), totalLength: chunk.totalLength };
      },
    };

    const reader = await TrajectoryReader.open(short);
    await expect(async () => {
      for await (const _frame of reader.walk()) {
        // The first request for frames is the one that comes up short.
      }
    }).rejects.toThrow(/bytes where/);
  });
});
