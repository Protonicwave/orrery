/// <reference lib="webworker" />

/**
 * The trajectory Worker.
 *
 * Everything between the network and a Float32Array happens here: the ranged
 * requests, the checksums, and the conversion of a component array into the
 * precision the device draws in. None of it happens on the main thread, which
 * is the whole point. Decoding a frame of eight thousand particles is a few
 * hundred microseconds and there are four hundred of them; done on the thread
 * that is also running the render loop, that is four hundred dropped frames
 * spread through the first minute, which reads as an instrument that stutters
 * rather than as one that is loading. ADR-0047.
 *
 * The frames leave here as transfers rather than copies, so a frame crosses
 * the boundary by having its ownership moved and not by being duplicated.
 */

import type { FromWorker, ToWorker } from './protocol';
import { TrajectoryReader } from './reader';
import { fetchSource } from './source';

const scope = self as unknown as DedicatedWorkerGlobalScope;

function post(message: FromWorker, transfer?: Transferable[]): void {
  scope.postMessage(message, transfer ?? []);
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

async function open(url: string): Promise<void> {
  let delivered = 0;

  try {
    const reader = await TrajectoryReader.open(fetchSource(url));
    post({
      kind: 'opened',
      count: reader.header.count,
      timestep: reader.header.timestep,
      frames: reader.frames,
      scalar: reader.header.scalar,
      velocities: reader.header.velocities,
      byteLength: reader.byteLength,
    });

    for await (const frame of reader.walk()) {
      post(
        {
          kind: 'frame',
          index: frame.index,
          step: frame.step,
          time: frame.time,
          x: frame.x,
          y: frame.y,
          z: frame.z,
        },
        [frame.x.buffer, frame.y.buffer, frame.z.buffer],
      );
      delivered += 1;
    }

    post({ kind: 'complete' });
  } catch (error) {
    post({ kind: 'failed', message: describe(error), delivered });
  }
}

scope.onmessage = (event: MessageEvent<ToWorker>) => {
  if (event.data.kind === 'open') void open(event.data.url);
};
