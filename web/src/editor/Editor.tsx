import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Masthead } from '../components/Masthead';
import { LiveRun } from '../solver/run';
import { describeDesign } from './description';
import { type Design, PRESETS, preset, problemsWith, writeDesign } from './design';
import { drawingOf } from './drawing';
import styles from './Editor.module.css';
import { Parameters } from './Parameters';
import {
  PreviewSurface,
  previewFrame,
  previewPlan,
  secondaryFrom,
  type View,
} from './preview';
import { Readout } from './Readout';
import { Sheet } from './Sheet';

/**
 * How long the editing has to stop before the preview is started again.
 *
 * A design is edited by dragging, and a slider moved across its travel produces
 * a hundred designs on the way. Sampling and stepping each of them would be a
 * hundred Workers started and killed to answer one movement, so the preview
 * waits for the movement to end. Long enough not to chase a drag, short enough
 * that letting go feels like the cause of what happens next.
 */
const SETTLE_MILLISECONDS = 400;

/**
 * The editor.
 *
 * A design, a drawing of it, the numbers it comes to, and the file it makes. The
 * drawing and the numbers are worked out in this tab from the settings; the
 * preview is the compiled solver sampling and stepping the same settings. So
 * there are two independent statements about the same design on one screen, and
 * they are meant to agree: the ellipse the drawing rules is where the sampler
 * puts the particles, and if it is not then one of the two is wrong.
 */
export function Editor() {
  const [design, setDesign] = useState<Design>(() => preset('galaxy-collision'));
  const drawing = useMemo(() => drawingOf(design), [design]);
  const text = useMemo(() => writeDesign(design, describeDesign), [design]);
  const problems = useMemo(() => problemsWith(design), [design]);

  // The preview. Null until it is asked for: the module is a download of a
  // hundred kilobytes and a Worker to run it in, and neither should happen to
  // somebody who came to draw a picture.
  const [previewing, setPreviewing] = useState(false);
  const [live, setLive] = useState<LiveRun | null>(null);
  const [reading, setReading] = useState(0);

  const canvasRef = useRef<HTMLCanvasElement>(null);
  const surfaceRef = useRef<PreviewSurface | null>(null);
  const viewRef = useRef<View | null>(null);

  const onView = useCallback((view: View) => {
    viewRef.current = view;
    surfaceRef.current?.place(view, window.devicePixelRatio);
  }, []);

  // The surface is made once, for the canvas the drawing put on the page, and
  // is told the colours from the stylesheet rather than holding its own. A
  // canvas cannot read a custom property, so the one place they are defined
  // stays the one place they are defined.
  useEffect(() => {
    const canvas = canvasRef.current;
    if (canvas === null) return;
    const surface = new PreviewSurface(canvas);
    const style = getComputedStyle(document.documentElement);
    surface.setInk(
      style.getPropertyValue('--ink').trim() || '#e6e2da',
      style.getPropertyValue('--brass').trim() || '#c8913f',
      style.getPropertyValue('--plate').trim() || '#000000',
    );
    surfaceRef.current = surface;
    if (viewRef.current !== null)
      surface.place(viewRef.current, window.devicePixelRatio);
    return () => {
      surfaceRef.current = null;
    };
  }, []);

  // Starting the preview again after the design has settled. The old run is
  // stopped first: two Workers stepping two designs would put one design's
  // particles under another design's drawing.
  useEffect(() => {
    if (!previewing) return;

    const timer = window.setTimeout(() => {
      setLive((current) => {
        current?.stop();
        const next = new LiveRun();
        next.start((limit) => previewPlan(design, limit));
        return next;
      });
    }, SETTLE_MILLISECONDS);

    return () => {
      window.clearTimeout(timer);
    };
  }, [design, previewing]);

  // Frames arrive at whatever rate the solver produces them, and the readout
  // beside the drawing follows how many there are rather than what is in them.
  useEffect(() => {
    if (live === null) return;
    return live.subscribe(() => {
      setReading(live.available);
    });
  }, [live]);

  // Which particle the second galaxy starts at. The design says how the count
  // is divided, but not what the count turned out to be: the module decides
  // that and reports it with the first frame, which is why this follows how
  // much has been read.
  // biome-ignore lint/correctness/useExhaustiveDependencies: the count arrives with the frames
  useEffect(() => {
    surfaceRef.current?.setSplit(secondaryFrom(design, live?.facts?.count ?? 0));
  }, [design, live, reading]);

  // The painting loop. It reads the newest frame and draws it, and allocates
  // nothing: the frame is a view onto memory the Worker transferred, and the
  // surface writes pixels rather than making objects.
  useEffect(() => {
    const surface = surfaceRef.current;
    if (live === null || surface === null) return;

    let handle = 0;
    let started = 0;
    const paint = (now: number): void => {
      if (started === 0) started = now;
      const frame = previewFrame(live, now - started);
      if (frame !== undefined) surface.draw(frame);
      handle = window.requestAnimationFrame(paint);
    };
    handle = window.requestAnimationFrame(paint);

    return () => {
      window.cancelAnimationFrame(handle);
    };
  }, [live]);

  useEffect(() => {
    if (previewing) return;
    surfaceRef.current?.clear();
  }, [previewing]);

  const startPreview = useCallback(() => {
    setPreviewing(true);
  }, []);

  const stopPreview = useCallback(() => {
    setPreviewing(false);
    setLive((current) => {
      current?.stop();
      return null;
    });
    setReading(0);
  }, []);

  // Stopping the Worker when the page goes, which a tab being closed does not
  // need and a navigation within the site does.
  useEffect(() => {
    return () => {
      live?.stop();
    };
  }, [live]);

  const onMove = useCallback((separation: number, impactParameter: number) => {
    setDesign((current) => ({
      ...current,
      separation: Math.max(0, Math.round(separation * 10) / 10),
      impactParameter: Math.round(impactParameter * 10) / 10,
    }));
  }, []);

  const scenario = PRESETS.find((entry) => entry.id === design.kind);

  return (
    <>
      <a className="skip-link" href="#drawing">
        Skip to the drawing
      </a>
      <div className={styles.editor}>
        <Masthead
          page="editor"
          state={live === null ? 'Designing' : 'Sampling'}
          build={
            <>
              {design.kind} · <em>{design.solver}</em> · v{__ORRERY_VERSION__}
            </>
          }
        />

        <div className={styles.stage} id="drawing">
          <div className={styles.sheet}>
            <div className={styles.caption}>
              <h1>{scenario?.title}</h1>
              <p>{scenario?.note}</p>
            </div>
            <Sheet
              design={design}
              drawing={drawing}
              onView={onView}
              canvasRef={canvasRef}
              onMove={design.kind === 'galaxy-collision' ? onMove : null}
            />
          </div>

          <Readout
            design={design}
            text={text}
            problems={problems}
            preview={{
              running: previewing,
              status: live?.status ?? null,
              message: live?.status === 'failed' ? live.message : '',
              plan: live?.plan ?? null,
              achieved: live?.achieved ?? null,
              onStart: startPreview,
              onStop: stopPreview,
            }}
          />
        </div>

        <Parameters design={design} onChange={setDesign} />
      </div>
    </>
  );
}
