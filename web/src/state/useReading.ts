/**
 * How much of the run has arrived, as a value React can render.
 *
 * A frame source is not a store: it is written to by a Worker's messages and it
 * decides for itself how often that is worth passing on. This turns its
 * subscription into one snapshot, taken in one place, so that the plate, the
 * transport and the instrument's text alternative are all describing the same
 * reading rather than each keeping their own copy of it.
 *
 * A published trajectory being decoded and a browser run being integrated are
 * both read through this, because both are a sequence of frames that grows
 * while it is being watched.
 */

import { useEffect, useState } from 'react';
import type { FrameSource, TrajectoryStatus } from '../trajectory/client';

export interface Reading {
  readonly status: TrajectoryStatus;
  /** Frames decoded so far, counting from the start and without a gap. */
  readonly available: number;
  /** Frames there will be altogether, once that is known. */
  readonly frames: number;
  /** Particles the source holds, which is the count that was actually drawn. */
  readonly count: number;
  /** Empty unless the read failed, and then a sentence saying how. */
  readonly message: string;
}

function snapshot(source: FrameSource): Reading {
  return {
    status: source.status,
    available: source.available,
    frames: source.facts?.frames ?? 0,
    count: source.facts?.count ?? 0,
    message: source.message,
  };
}

export function useReading(source: FrameSource): Reading {
  const [reading, setReading] = useState(() => snapshot(source));

  useEffect(() => {
    setReading(snapshot(source));
    return source.subscribe(() => {
      setReading(snapshot(source));
    });
  }, [source]);

  return reading;
}
