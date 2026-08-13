import { useEffect, useRef } from 'react';
import { drawSeries, plotRange } from '../diagnostics/plot';
import { extent, sampleAt } from '../diagnostics/series';
import { type Reading, TIMES } from '../format/number';
import type { InstantSource } from '../state/instant';
import styles from './Diagnostic.module.css';

export interface DiagnosticProps {
  /** What the quantity is called, in words. */
  name: string;
  /** The samples, in the order the run wrote them. */
  values: Float64Array;
  /** The model time each sample was taken at. */
  times: Float64Array;
  /** The interval of model time the run covers, which the cursor spans. */
  modelTime: number;
  instants: InstantSource;
  /** The note under the left of the plot: what the range is, in words. */
  note: string;
  /** The symbol under the right of it, which is what the quantity is. */
  symbol: string;
  format: (value: number) => Reading;
}

/**
 * One diagnostic: a name, a sparkline, the value at the instant being read.
 *
 * Direct-labelled and unlabelled at once. The value belongs to the moment the
 * plate is showing, so it moves as the run plays and the brass cursor moves
 * with it; between them they say where on the curve the picture is, which is
 * what a legend would otherwise have to say in words.
 *
 * Nothing here re-renders while a run plays. The plot is drawn when the run
 * changes and when the box is resized; the cursor and the value are written
 * into elements by a subscription to the instant, ten times a second, at the
 * cost of two style writes and three assignments.
 */
export function Diagnostic({
  name,
  values,
  times,
  modelTime,
  instants,
  note,
  symbol,
  format,
}: DiagnosticProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const cursorRef = useRef<HTMLSpanElement>(null);
  const figureRef = useRef<HTMLSpanElement>(null);
  const timesRef = useRef<HTMLSpanElement>(null);
  const exponentRef = useRef<HTMLElement>(null);
  const spokenRef = useRef<HTMLSpanElement>(null);

  // Redrawn when the samples change and when the rail is resized, which is the
  // whole set of occasions on which the line moves.
  useEffect(() => {
    const canvas = canvasRef.current;
    if (canvas === null) return;

    const { low, high } = extent(values);
    const range = plotRange(low, high);
    const draw = () => {
      drawSeries(canvas, values, range);
    };

    draw();
    const observer = new ResizeObserver(draw);
    observer.observe(canvas);
    return () => {
      observer.disconnect();
    };
  }, [values]);

  useEffect(
    () =>
      instants.subscribe((time) => {
        const cursor = cursorRef.current;
        if (cursor !== null) {
          const fraction =
            modelTime === 0 ? 0 : Math.min(Math.max(time / modelTime, 0), 1);
          cursor.style.left = `${fraction * 100}%`;
        }

        const index = sampleAt(times, time);
        if (index < 0) return;
        const reading = format(values[index] as number);
        if (figureRef.current !== null) figureRef.current.textContent = reading.figure;
        if (timesRef.current !== null) {
          timesRef.current.textContent = reading.exponent === '' ? '' : `${TIMES}10`;
        }
        if (exponentRef.current !== null) {
          exponentRef.current.textContent = reading.exponent;
        }
        if (spokenRef.current !== null) spokenRef.current.textContent = reading.spoken;
      }),
    [instants, times, values, modelTime, format],
  );

  return (
    <div className={styles.diagnostic}>
      <div className={styles.head}>
        <span className={styles.name}>{name}</span>
        <span className={styles.value}>
          <span aria-hidden="true">
            <span ref={figureRef} />
            <span ref={timesRef} />
            <sup className={styles.exponent} ref={exponentRef} />
          </span>
          <span className="visually-hidden" ref={spokenRef} />
        </span>
      </div>

      {/* The line and the cursor carry nothing the head and the foot do not,
          so they are furniture to a reader who cannot see them. */}
      <div className={styles.frame} aria-hidden="true">
        <canvas className={styles.canvas} ref={canvasRef} />
        <span className={styles.cursor} ref={cursorRef} />
      </div>

      <div className={styles.foot}>
        <span>{note}</span>
        <span>{symbol}</span>
      </div>
    </div>
  );
}
