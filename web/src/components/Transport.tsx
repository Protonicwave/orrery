import type { Run } from '../config/run';
import { decimal } from '../format/number';
import { Numeric } from './Numeric';
import styles from './Transport.module.css';

export interface TransportProps {
  run: Run;
  /** Where in the run the instrument is reading, in model time. */
  time: number;
  playing: boolean;
  onPlayingChange: (playing: boolean) => void;
  onSeek: (time: number) => void;
}

/**
 * Ticks at the diagnostics stride, marked every tenth. Fixed furniture, built
 * once, so drawing them costs nothing per render and each has a name of its
 * own rather than a position in a list.
 */
const TICKS = Array.from({ length: 41 }, (_, index) => ({
  id: `tick-${index}`,
  major: index % 10 === 0,
}));

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
  playing,
  onPlayingChange,
  onSeek,
}: TransportProps) {
  const fraction = run.modelTime === 0 ? 0 : time / run.modelTime;
  const step = Math.round(time / run.timestep);

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
          {TICKS.map((tick) => (
            <span key={tick.id} className={tick.major ? styles.major : undefined} />
          ))}
        </span>
        <span className={styles.cursor} style={{ left: `${fraction * 100}%` }} />
        <input
          type="range"
          className={styles.range}
          min={0}
          max={run.modelTime}
          step={run.timestep}
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
        <p className={styles.statistic}>
          <span className="label">Bodies</span>
          <Numeric value={run.count} />
        </p>
        <p className={styles.statistic}>
          <span className="label">Frames</span>
          <Numeric value={run.frames} />
        </p>
      </div>
    </div>
  );
}
