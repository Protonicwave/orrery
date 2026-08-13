import { useEffect, useMemo, useRef } from 'react';
import { createProfile, measureProfile } from '../diagnostics/profile';
import { setScientific, TIMES } from '../format/number';
import type { InstantSource } from '../state/instant';
import type { Trajectory } from '../trajectory/client';
import styles from './Diagnostic.module.css';

export interface ProfileProps {
  trajectory: Trajectory;
  instants: InstantSource;
}

/** Retina and no further, as the sparklines are drawn. */
const MAXIMUM_RATIO = 2;

/**
 * The radial mass profile of the frame being drawn.
 *
 * Set like a diagnostic, and it is not one: the four above it are what the run
 * measured, and this is what the client worked out from the frame on the plate.
 * The difference is stated in the note under it rather than left to be noticed,
 * because a plot in a rail of measurements reads as a measurement.
 *
 * Both axes are logarithmic, and there is no cursor: this plot is the instant,
 * where the others are the run. It is redrawn each time the instant is
 * published, which is ten times a second, and the arithmetic behind it is a
 * pass over the frame that allocates nothing.
 */
export function Profile({ trajectory, instants }: ProfileProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const peakRef = useRef<HTMLSpanElement>(null);
  const timesRef = useRef<HTMLSpanElement>(null);
  const exponentRef = useRef<HTMLElement>(null);
  const spokenRef = useRef<HTMLSpanElement>(null);
  const profile = useMemo(() => createProfile(), []);

  useEffect(
    () =>
      instants.subscribe((time) => {
        const canvas = canvasRef.current;
        const frame = trajectory.frameAt(time);
        const masses = trajectory.facts?.masses;
        if (canvas === null || frame === undefined || masses === undefined) return;

        measureProfile(frame.x, frame.y, frame.z, masses, profile);

        const reading = setScientific(profile.peak);
        if (peakRef.current !== null) peakRef.current.textContent = reading.figure;
        if (timesRef.current !== null) {
          timesRef.current.textContent = reading.exponent === '' ? '' : `${TIMES}10`;
        }
        if (exponentRef.current !== null) {
          exponentRef.current.textContent = reading.exponent;
        }
        if (spokenRef.current !== null) spokenRef.current.textContent = reading.spoken;

        draw(canvas, profile.radius, profile.density, profile.occupied, profile.peak);
      }),
    [instants, trajectory, profile],
  );

  return (
    <div className={styles.diagnostic}>
      <div className={styles.head}>
        <span className={styles.name}>Radial density</span>
        <span className={styles.value}>
          <span aria-hidden="true">
            <span ref={peakRef} />
            <span ref={timesRef} />
            <sup className={styles.exponent} ref={exponentRef} />
          </span>
          <span className="visually-hidden" ref={spokenRef} />
        </span>
      </div>

      <div className={styles.frame} aria-hidden="true">
        <canvas className={styles.canvas} ref={canvasRef} />
      </div>

      <div className={styles.foot}>
        <span>derived, both axes log</span>
        <span>ρ(r)</span>
      </div>
    </div>
  );
}

/**
 * The profile as a line, scaled to its own peak and to five decades below it.
 *
 * Five decades because a Plummer sphere falls as the inverse fifth power of
 * radius outside its scale radius, so anything shallower than that is inside
 * the model and anything below it is the handful of particles an encounter
 * threw furthest.
 */
function draw(
  canvas: HTMLCanvasElement,
  radius: Float64Array,
  density: Float64Array,
  occupied: number,
  peak: number,
): void {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (width === 0 || height === 0 || occupied === 0 || peak <= 0) return;

  const ratio = Math.min(window.devicePixelRatio || 1, MAXIMUM_RATIO);
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);

  const context = canvas.getContext('2d');
  if (context === null) return;
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);

  const decades = 5;
  const floor = peak * 10 ** -decades;
  const outermost = radius[occupied - 1] as number;
  const innermost = radius[0] as number;
  const span = Math.log(outermost / innermost);
  if (!(span > 0)) return;

  context.strokeStyle = getComputedStyle(canvas).getPropertyValue('--ink-2').trim();
  context.lineWidth = 1;
  context.beginPath();

  let started = false;
  for (let shell = 0; shell < occupied; shell += 1) {
    const value = density[shell] as number;
    if (value <= floor) {
      // A shell with nothing in it is a gap in the profile rather than a
      // density of zero, and drawing through it would invent a line where the
      // frame has no particles.
      started = false;
      continue;
    }
    const px = (Math.log((radius[shell] as number) / innermost) / span) * width;
    const py = height - (Math.log10(value / floor) / decades) * height;
    if (started) context.lineTo(px, py);
    else context.moveTo(px, py);
    started = true;
  }
  context.stroke();
}
