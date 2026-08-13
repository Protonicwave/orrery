import { useMemo } from 'react';
import type { Run } from '../config/run';
import { decimal } from '../format/number';
import { Numeric } from './Numeric';
import styles from './Transport.module.css';

export interface TransportProps {
  run: Run;
  /** Where in the run the instrument is reading, in model time. */
  time: number;
  /** Particles in the trajectory being drawn, which is the file's own figure. */
  bodies: number;
  playing: boolean;
  onPlayingChange: (playing: boolean) => void;
  onSeek: (time: number) => void;
}

/**
 * As many ticks as the run has diagnostics samples, because that is where the
 * data the track scrubs over actually exists: a tick is an instant the rail can
 * plot a value at.
 *
 * Thinned when the stride is fine enough to put them closer together than a
 * hairline apart, in which case they stop being marks and become a grey band.
 * A run's stride is a setting, so this cannot be decided once and written down.
 */
const MOST_TICKS = 130;

function ticksFor(samples: number): { id: string; major: boolean }[] {
  if (samples <= 1) return [];
  const thinning = Math.max(1, Math.ceil(samples / MOST_TICKS));
  const count = Math.floor((samples - 1) / thinning) + 1;
  return Array.from({ length: count }, (_, index) => ({
    id: `tick-${index}`,
    major: index % 10 === 0,
  }));
}

/**
 * The transport: where in the run the instrument is reading.
 *
 * The track is a range input under its appearance, which is what gives it the
 * arrow keys, Page Up and Page Down, Home and End, a reported minimum and
 * maximum and a spoken value, none of which a div and a pointer handler would
 * have had.
 *
 * The step beside the clock is not a second piece of state: a run is a number
 * of steps and a timestep, so the step is the time divided by one of them, and
 * the two readouts cannot disagree.
 */
export function Transport({
  run,
  time,
  bodies,
  playing,
  onPlayingChange,
  onSeek,
}: TransportProps) {
  const fraction = run.modelTime === 0 ? 0 : time / run.modelTime;
  const step = Math.round(time / run.timestep);
  const ticks = useMemo(() => ticksFor(run.samples), [run.samples]);

  // The track moves by one trajectory frame, not by one integrator step. A
  // trajectory is written at a stride, so the instants between two frames are
  // instants the run did not record and the plate cannot show; a control that
  // offered them would answer an arrow key by not changing the picture.
  const interval = run.frames > 1 ? run.modelTime / (run.frames - 1) : run.timestep;

  return (
    <div className={styles.transport}>
      <button
        type="button"
        className={styles.button}
        aria-pressed={playing}
        onClick={() => onPlayingChange(!playing)}
      >
        {playing ? (
          <svg width="9" height="10" viewBox="0 0 9 10" aria-hidden="true">
            <path d="M0 0 H3 V10 H0 Z M6 0 H9 V10 H6 Z" fill="currentColor" />
          </svg>
        ) : (
          <svg width="9" height="10" viewBox="0 0 9 10" aria-hidden="true">
            <path d="M0 0 L9 5 L0 10 Z" fill="currentColor" />
          </svg>
        )}
        <span className="visually-hidden">{playing ? 'Pause' : 'Play'}</span>
      </button>

      <p className={styles.clock}>
        <Numeric value={time} digits={3} />
        <small>
          {' / '}
          <Numeric value={run.modelTime} digits={3} />
        </small>
      </p>

      <div className={styles.track}>
        <span className={styles.line} />
        <span className={styles.fill} style={{ width: `${fraction * 100}%` }} />
        <span className={styles.ticks} aria-hidden="true">
          {ticks.map((tick) => (
            <span key={tick.id} className={tick.major ? styles.major : undefined} />
          ))}
        </span>
        <span className={styles.cursor} style={{ left: `${fraction * 100}%` }} />
        <input
          type="range"
          className={styles.range}
          min={0}
          max={run.modelTime}
          step={interval}
          value={time}
          aria-label="Position in the run"
          aria-valuetext={`model time ${decimal(time, 3)} of ${decimal(run.modelTime, 3)}, step ${decimal(step)}`}
          onChange={(event) => onSeek(Number(event.target.value))}
        />
      </div>

      <div className={styles.statistics}>
        <p className={styles.statistic}>
          <span className="label">Step</span>
          <Numeric value={step} />
        </p>
        {/* The count the file holds rather than the count the configuration
            asked for. They agree, and where the interface has both it shows
            the one that was actually drawn. */}
        <p className={styles.statistic}>
          <span className="label">Bodies</span>
          <Numeric value={bodies} />
        </p>
        <p className={styles.statistic}>
          <span className="label">Frames</span>
          <Numeric value={run.frames} />
        </p>
      </div>
    </div>
  );
}
