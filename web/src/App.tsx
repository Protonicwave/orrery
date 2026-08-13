import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import styles from './App.module.css';
import { Console } from './components/Console';
import { Masthead } from './components/Masthead';
import { Plate } from './components/Plate';
import { Rail } from './components/Rail';
import { Transport } from './components/Transport';
import { numeric } from './config/parse';
import { collision } from './config/run';
import { type Diagnostics, fetchDiagnostics } from './diagnostics/series';
import { diagnosticsUrl, GALLERY, trajectoryUrl } from './gallery/runs';
import type { Instrument } from './render/instrument';
import { InstantSource } from './state/instant';
import { useViewerShortcuts } from './state/shortcuts';
import { createChromeState } from './state/store';
import { useReading } from './state/useReading';
import { useStoreValue } from './state/useStore';
import { Trajectory } from './trajectory/client';

/**
 * The instrument.
 *
 * The run is fixed for now: the collision the repository demonstrates, read out
 * of its own configuration file and played from the trajectory that
 * configuration produced. Which run is loaded becomes a property of the address
 * when the gallery has more than one in it.
 */
export function App() {
  const run = collision;
  const softening = numeric(run.configuration, 'solver', 'softening') ?? 0;
  const chrome = useMemo(
    () =>
      createChromeState({
        count: run.count,
        softening,
        integrator: 'velocity-verlet',
      }),
    [softening],
  );

  // The reader is started once, here, rather than by the plate. A backend that
  // will not start is a reason to say so on the plate and not a reason to stop
  // reading the run: the transport, the diagnostics and the configuration are
  // all still worth having.
  const trajectory = useMemo(() => new Trajectory(), []);
  useEffect(() => {
    const published = GALLERY[0];
    if (published !== undefined) {
      trajectory.open(trajectoryUrl(import.meta.env.BASE_URL, published));
    }
    return () => {
      trajectory.close();
    };
  }, [trajectory]);

  // The run's own diagnostics, fetched beside its trajectory. A run whose
  // diagnostics will not load still plays: the plate and the transport need
  // nothing from this file, so the rail says what went wrong and the rest of
  // the instrument carries on.
  const [diagnostics, setDiagnostics] = useState<Diagnostics | null>(null);
  const [unreadable, setUnreadable] = useState('');
  useEffect(() => {
    const published = GALLERY[0];
    if (published === undefined) return;

    let live = true;
    void fetchDiagnostics(diagnosticsUrl(import.meta.env.BASE_URL, published))
      .then((read) => {
        if (live) setDiagnostics(read);
      })
      .catch((error: unknown) => {
        if (!live) return;
        setUnreadable(
          error instanceof Error ? error.message : 'the diagnostics would not load',
        );
      });
    return () => {
      live = false;
    };
  }, []);

  // The instant being read. Written ten times a second by the render loop
  // rather than sixty, which is the rate a clock can be read at and the reason
  // a rendered instant never reaches React on a frame.
  //
  // The transport's clock is React state because its track is a controlled
  // input. Everything else that follows the instant, which is four sparkline
  // cursors and the four values labelled above them, subscribes to the source
  // below and writes into the document instead.
  const [time, setTime] = useState(0);
  const instants = useMemo(() => new InstantSource(), []);
  const sample = useCallback(
    (at: number, step: number) => {
      setTime(at);
      instants.publish(at, step);
    },
    [instants],
  );
  const playing = useStoreValue(chrome, (state) => state.playing);
  const showDiagnostics = useStoreValue(chrome, (state) => state.diagnostics);

  // How much of the run has been read, taken once and given to everything that
  // describes it, so the plate's particle count and the transport's cannot be
  // two answers arrived at separately.
  const reading = useReading(trajectory);

  // The transport moves the render loop, which owns where playback is. The
  // reference is filled in by the plate once a backend has started, and stays
  // null if none does, which is what makes the transport a control that says
  // it can do nothing rather than one that appears to work.
  const instrument = useRef<Instrument | null>(null);

  // The native viewer's keys, from anywhere on the page rather than only from
  // the plate. The plate keeps its own handler because it also turns the camera.
  useViewerShortcuts({
    onPlayPause: useCallback(() => {
      chrome.set((state) => ({ playing: !state.playing }));
    }, [chrome]),
    onReframe: useCallback(() => {
      instrument.current?.reframe();
    }, []),
    onExposure: useCallback(
      (factor: number) => {
        chrome.set((state) => ({ exposure: state.exposure * factor }));
      },
      [chrome],
    ),
  });

  return (
    <>
      <a className="skip-link" href="#plate">
        Skip to the instrument
      </a>
      <div className={styles.instrument}>
        <Masthead run={run} />
        <div className={styles.stage}>
          <Plate
            run={run}
            trajectory={trajectory}
            reading={reading}
            chrome={chrome}
            instrumentRef={instrument}
            onSample={sample}
          />
          <Rail
            run={run}
            diagnostics={diagnostics}
            instants={instants}
            showDiagnostics={showDiagnostics}
            message={unreadable}
          />
        </div>
        <Transport
          run={run}
          time={time}
          bodies={reading.count}
          playing={playing}
          onPlayingChange={(next) => chrome.set({ playing: next })}
          onSeek={(next) => {
            sample(next, Math.round(next / run.timestep));
            instrument.current?.seekTime(next);
          }}
        />
        <Console run={run} chrome={chrome} />
      </div>
    </>
  );
}
