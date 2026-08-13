import { type RefObject, useEffect, useRef, useState } from 'react';
import type { Run } from '../config/run';
import { createRenderer } from '../render/create';
import { createFurniture } from '../render/furniture';
import { Instrument } from '../render/instrument';
import { DEFAULT_SETTINGS, type RenderSettings } from '../render/renderer';
import type { ChromeState, Store } from '../state/store';
import { createFrameState } from '../state/store';
import type { Reading } from '../state/useReading';
import { useStoreState } from '../state/useStore';
import type { Trajectory } from '../trajectory/client';
import { Numeric } from './Numeric';
import styles from './Plate.module.css';

export interface PlateProps {
  run: Run;
  trajectory: Trajectory;
  /** How much of the run has been read, taken once and shared. */
  reading: Reading;
  chrome: Store<ChromeState>;
  /** Filled in with the render loop once a backend has started. */
  instrumentRef: RefObject<Instrument | null>;
  /** Told what instant is on the plate, ten times a second and no more. */
  onSample: (time: number, step: number) => void;
}

/** What the plate is doing, in the order it does it. */
type Condition = 'starting' | 'exposing' | 'failed';

/**
 * The exposure as a camera states it, which is a number of stops above or
 * below a gain of one rather than the gain itself. A photographer reads +0.68
 * and knows what half a stop more would look like; 1.60 is a multiplier.
 */
function stops(exposure: number): number {
  return exposure <= 0 ? 0 : Math.log2(exposure);
}

/** What the two view controls mean to the renderer. */
function settingsFor(exposure: number, spriteRadius: number): RenderSettings {
  return {
    ...DEFAULT_SETTINGS,
    exposure,
    pointSize: DEFAULT_SETTINGS.pointSize * spriteRadius,
  };
}

/**
 * What the canvas says to a reader who cannot see it.
 *
 * A picture of a galaxy is neither decoration nor readable, so it carries a
 * description of the run, of how much of it has been read, and of what can be
 * done to it. The last sentence is there because the canvas is a control and
 * its keys are not discoverable by looking at it.
 */
function describe(run: Run, count: number, available: number, frames: number): string {
  return (
    `${run.name}: ${count} particles, ${run.published.title.toLowerCase()}, ` +
    'drawn as a field of points on a black plate. ' +
    `${available} of ${frames} frames read. ` +
    'Drag to turn, scroll to zoom, or use the arrow keys.'
  );
}

/**
 * The plate: the surface a run is exposed onto.
 *
 * The canvas is not rendered by React. It is created by whichever backend
 * starts, appended to the container below, and driven from
 * `render/instrument.ts` at sixty hertz. Everything React owns here changes at
 * the speed a person changes it: the catalogue in the corners, the exposure,
 * and the sentence the plate carries while it has nothing on it yet.
 *
 * The catalogue is written the way a plate is written on: which run, which
 * configuration, which seed, and the conditions the exposure was taken under.
 * The device and the particle count are in it because a picture drawn in a
 * browser must not be mistaken for one of the native figures, and the only way
 * to prevent that is to state what actually drew it.
 */
export function Plate({
  run,
  trajectory,
  reading,
  chrome,
  instrumentRef,
  onSample,
}: PlateProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const scaleBarRef = useRef<HTMLSpanElement>(null);
  const scaleLabelRef = useRef<HTMLSpanElement>(null);
  // Three axes, named rather than indexed. A list would need a hook per entry
  // and an index into it at every use, and there are exactly three.
  const axisX = useRef<SVGLineElement>(null);
  const axisY = useRef<SVGLineElement>(null);
  const axisZ = useRef<SVGLineElement>(null);
  const labelX = useRef<SVGTextElement>(null);
  const labelY = useRef<SVGTextElement>(null);
  const labelZ = useRef<SVGTextElement>(null);

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const sampleRef = useRef(onSample);
  sampleRef.current = onSample;

  const [condition, setCondition] = useState<Condition>('starting');
  const [message, setMessage] = useState('');
  const [device, setDevice] = useState('');

  const state = useStoreState(chrome);

  // Starting a backend is asynchronous and a component can be unmounted while
  // it is happening, so the effect carries a flag rather than assuming the
  // element it was given is still on the page when the device answers.
  useEffect(() => {
    const container = containerRef.current;
    if (container === null) return;

    let live = true;
    let instrument: Instrument | null = null;

    void (async () => {
      try {
        // `?renderer=webgl2` pins the backend. It exists so that the two can be
        // compared: a screenshot of one against a screenshot of the other is
        // what says they draw the same picture, and that cannot be taken if the
        // machine is the one deciding which of them runs.
        const asked = new URLSearchParams(window.location.search).get('renderer');
        const started = await createRenderer(
          container,
          styles.canvas as string,
          asked === 'webgl2' ? 'webgl2' : 'webgpu',
        );
        if (!live) {
          started.renderer.dispose();
          return;
        }

        const bar = scaleBarRef.current;
        const barLabel = scaleLabelRef.current;
        const furniture =
          bar !== null &&
          barLabel !== null &&
          axisX.current !== null &&
          axisY.current !== null &&
          axisZ.current !== null &&
          labelX.current !== null &&
          labelY.current !== null &&
          labelZ.current !== null
            ? createFurniture({
                scaleBar: bar,
                scaleLabel: barLabel,
                axes: [axisX.current, axisY.current, axisZ.current],
                axisLabels: [labelX.current, labelY.current, labelZ.current],
              })
            : null;

        // The controls already have values when the backend finishes starting,
        // so the instrument is built with them rather than with the defaults
        // and corrected a frame later.
        const opening = chrome.get();
        instrument = new Instrument({
          container,
          canvas: started.canvas,
          renderer: started.renderer,
          trajectory,
          frame: createFrameState(),
          settings: settingsFor(opening.exposure, opening.spriteRadius),
          furniture,
          onExposure: (exposure) => chrome.set({ exposure }),
          onPlaying: (playing) => chrome.set({ playing }),
          onSample: (time, step) => sampleRef.current(time, step),
        });
        instrument.playing = opening.playing;

        started.canvas.setAttribute(
          'aria-label',
          describe(run, trajectory.facts?.count ?? 0, trajectory.available, 0),
        );

        instrumentRef.current = instrument;
        canvasRef.current = started.canvas;
        setDevice(started.renderer.description);
        setCondition('exposing');
        instrument.start();
      } catch (error) {
        if (!live) return;
        setCondition('failed');
        setMessage(error instanceof Error ? error.message : String(error));
      }
    })();

    return () => {
      live = false;
      instrument?.dispose();
      instrumentRef.current = null;
      canvasRef.current = null;
    };
  }, [chrome, trajectory, instrumentRef, run]);

  // Chrome state reaches the render loop by being written onto the instrument,
  // not by being read through it. A settings object is replaced on a change
  // rather than mutated, which happens when a control is operated and never on
  // a frame.
  useEffect(() => {
    const instrument = instrumentRef.current;
    if (instrument === null) return;
    instrument.settings = settingsFor(state.exposure, state.spriteRadius);
    instrument.playing = state.playing;
  }, [instrumentRef, state.exposure, state.spriteRadius, state.playing]);

  // The text alternative follows what has been read. Its first value is set
  // where the canvas is made, because this effect cannot run before there is a
  // canvas and the reading may not change again after there is one.
  useEffect(() => {
    canvasRef.current?.setAttribute(
      'aria-label',
      describe(run, reading.count, reading.available, reading.frames),
    );
  }, [run, reading]);

  const share =
    reading.frames === 0 ? 0 : Math.round((reading.available / reading.frames) * 100);

  return (
    <section className={styles.plate} id="plate" aria-labelledby="plate-heading">
      <h2 className="visually-hidden" id="plate-heading">
        The plate
      </h2>

      <div className={styles.surface} ref={containerRef} />

      <span className={`${styles.registration} ${styles.topLeft}`} />
      <span className={`${styles.registration} ${styles.topRight}`} />
      <span className={`${styles.registration} ${styles.bottomLeft}`} />
      <span className={`${styles.registration} ${styles.bottomRight}`} />

      <dl className={`${styles.note} ${styles.left}`}>
        <div>
          <dt>RUN</dt> <dd>{run.name}</dd>
        </div>
        <div>
          <dt>CFG</dt> <dd>{run.path}</dd>
        </div>
        <div>
          <dt>SEED</dt> <dd>{run.seed}</dd>
        </div>
      </dl>

      <dl className={`${styles.note} ${styles.right}`}>
        <div>
          <dt>EV</dt>{' '}
          <dd>
            <Numeric value={stops(state.exposure)} notation="signed" />
          </dd>
        </div>
        <div>
          <dt>PROJ</dt> <dd>perspective 40°</dd>
        </div>
        <div>
          <dt>N</dt>{' '}
          <dd className={styles.value}>
            <Numeric value={reading.count} />
          </dd>
        </div>
        <div>
          <dt>DEV</dt>{' '}
          <dd className={styles.device} title={device}>
            {device === '' ? 'starting' : device}
          </dd>
        </div>
      </dl>

      {/* The scale bar: a length on the screen that can be read as a length in
          the model. G = 1 is stated beside it because a length in these units
          only means something with the unit system named. ADR-0007. */}
      <div className={styles.scale} aria-hidden="true">
        <span className={styles.scaleBar} ref={scaleBarRef} />
        <span className={styles.scaleText}>
          <span ref={scaleLabelRef} />
          <small> G = 1</small>
        </span>
      </div>

      {/* The gnomon: which way the axes point after a drag has lost them. */}
      <svg
        className={styles.gnomon}
        viewBox="-30 -30 60 60"
        width="60"
        height="60"
        aria-hidden="true"
      >
        <title>The orientation of the model axes</title>
        <line ref={axisX} x1="0" y1="0" x2="0" y2="0" stroke="currentColor" />
        <line ref={axisY} x1="0" y1="0" x2="0" y2="0" stroke="currentColor" />
        <line ref={axisZ} x1="0" y1="0" x2="0" y2="0" stroke="currentColor" />
        <text ref={labelX} x="0" y="0">
          x
        </text>
        <text ref={labelY} x="0" y="0">
          y
        </text>
        <text ref={labelZ} x="0" y="0">
          z
        </text>
      </svg>

      {condition === 'failed' && (
        <p className={styles.unexposed}>
          {message} The method pages describe the run this plate would have shown.
        </p>
      )}

      {condition !== 'failed' && reading.available === 0 && (
        <p className={styles.unexposed}>
          {reading.status === 'failed' ? reading.message : 'The plate is being exposed'}
        </p>
      )}

      {condition !== 'failed' && reading.available > 0 && share < 100 && (
        <p className={styles.reading} role="status">
          Reading the run, {share} per cent
        </p>
      )}
    </section>
  );
}
