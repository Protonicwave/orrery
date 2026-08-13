/**
 * How much of the run has been read, as a value React can render.
 *
 * The trajectory client is not a store: it is written to by a Worker's messages
 * and it notifies a hundred times over a load rather than four hundred, which
 * is its own decision and the reason it has a subscription of its own. This
 * turns that subscription into one snapshot, taken in one place, so that the
 * plate, the transport and the instrument's text alternative are all describing
 * the same reading rather than each keeping their own copy of it.
 */

import { useEffect, useState } from 'react';
import type { Trajectory, TrajectoryStatus } from '../trajectory/client';

export interface Reading {
  readonly status: TrajectoryStatus;
  /** Frames decoded so far, counting from the start and without a gap. */
  readonly available: number;
  /** Frames the file holds, once its header has been read. */
  readonly frames: number;
  /** Particles in the file, which is the count that was actually drawn. */
  readonly count: number;
  /** Empty unless the read failed, and then a sentence saying how. */
  readonly message: string;
}

function snapshot(trajectory: Trajectory): Reading {
  return {
    status: trajectory.status,
    available: trajectory.available,
    frames: trajectory.facts?.frames ?? 0,
    count: trajectory.facts?.count ?? 0,
    message: trajectory.message,
  };
}

export function useReading(trajectory: Trajectory): Reading {
  const [reading, setReading] = useState(() => snapshot(trajectory));

  useEffect(() => {
    setReading(snapshot(trajectory));
    return trajectory.subscribe(() => {
      setReading(snapshot(trajectory));
    });
  }, [trajectory]);

  return reading;
}
