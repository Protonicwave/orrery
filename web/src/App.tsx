import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import styles from './App.module.css';
import { Console } from './components/Console';
import { Gallery } from './components/Gallery';
import { Masthead } from './components/Masthead';
import { Plate } from './components/Plate';
import { Rail } from './components/Rail';
import { Transport } from './components/Transport';
import { numeric } from './config/parse';
import { RUNS, runFor } from './config/run';
import { type Diagnostics, fetchDiagnostics } from './diagnostics/series';
import { readAddress, writeAddress } from './gallery/address';
import { diagnosticsUrl, type GalleryRun, trajectoryUrl } from './gallery/runs';
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
 * Which run is loaded and where the transport is are both properties of the
 * address, so a run and a moment in it can be sent to somebody. Everything else
 * on screen follows from the run: the trajectory being drawn, the diagnostics
 * being plotted, and the configuration in the rail are one run's three files
 * and are replaced together.
 */
export function App() {
  const opening = useMemo(() => readAddress(window.location.search), []);
  const [published, setPublished] = useState<GalleryRun>(opening.run);
  const run = runFor(published);

  // The moment the address named, until the transport has been able to reach
  // it. A reference rather than state: nothing renders differently for it, and
  // it is cleared from inside an effect that would otherwise re-run itself.
  const wanted = useRef<number | null>(opening.time);

  const softening = numeric(run.configuration, 'solver', 'softening') ?? 0;
  const chrome = useMemo(
    () =>
      createChromeState({
        count: run.count,
        softening,
        integrator: 'velocity-verlet',
      }),
    [run.count, softening],
  );

  // The reader is started once per run, here, rather than by the plate. A
  // backend that will not start is a reason to say so on the plate and not a
  // reason to stop reading the run: the transport, the diagnostics and the
  // configuration are all still worth having.
  // The dependency is the point rather than an oversight: a new run is a new
  // reader, and the identifier is what says the run has changed. The rule
  // assumes a memo's dependencies are the values it reads.
  // biome-ignore lint/correctness/useExhaustiveDependencies: one reader per run
  const trajectory = useMemo(() => new Trajectory(), [published.id]);
  useEffect(() => {
    trajectory.open(trajectoryUrl(import.meta.env.BASE_URL, published));
    return () => {
      trajectory.close();
    };
  }, [trajectory, published]);

  // The run's own diagnostics, fetched beside its trajectory. A run whose
  // diagnostics will not load still plays: the plate and the transport need
  // nothing from this file, so the rail says what went wrong and the rest of
  // the instrument carries on.
  const [diagnostics, setDiagnostics] = useState<Diagnostics | null>(null);
  const [unreadable, setUnreadable] = useState('');
  useEffect(() => {
    let live = true;
    setDiagnostics(null);
    setUnreadable('');

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
  }, [published]);

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
  const showProfile = useStoreValue(chrome, (state) => state.radialProfile);

  // How much of the run has been read, taken once and given to everything that
  // describes it, so the plate's particle count and the transport's cannot be
  // two answers arrived at separately.
  const reading = useReading(trajectory);

  // The transport moves the render loop, which owns where playback is. The
  // reference is filled in by the plate once a backend has started, and stays
  // null if none does, which is what makes the transport a control that says
  // it can do nothing rather than one that appears to work.
  const instrument = useRef<Instrument | null>(null);

  // A moment named by the address is reached as soon as the frame for it has
  // been decoded, which for a moment near the end of a long run is not when
  // the page loads. Tried again on each report of how much has arrived.
  useEffect(() => {
    const at = wanted.current;
    if (at === null || reading.available === 0) return;
    // Nothing is reported here. The address names a moment and the instrument
    // goes to the nearest frame the run recorded, which is a different number;
    // the loop publishes that frame's own time on its next sample, and the
    // clock should show the instant being drawn rather than the one asked for.
    if (instrument.current?.seekTime(at) === true) wanted.current = null;
  }, [reading.available]);

  const choose = useCallback((next: GalleryRun) => {
    window.history.pushState(null, '', writeAddress(window.location.search, next, 0));
    wanted.current = null;
    setPublished(next);
    setTime(0);
  }, []);

  // Back and forward are navigations between runs and between moments, so they
  // have to be answered rather than left to change the address under a page
  // that does not notice.
  useEffect(() => {
    const onPopState = (): void => {
      const address = readAddress(window.location.search);
      wanted.current = address.time;
      setPublished(address.run);
    };
    window.addEventListener('popstate', onPopState);
    return () => {
      window.removeEventListener('popstate', onPopState);
    };
  }, []);

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
        <Gallery runs={RUNS} current={published} onChoose={choose} />
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
            trajectory={trajectory}
            showDiagnostics={showDiagnostics}
            showProfile={showProfile}
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
            // The address follows the transport when it is moved by hand and
            // not while a run is playing. A moment worth sending someone is one
            // that was chosen, and rewriting the address sixty times a second
            // would be a history nobody could go back through.
            window.history.replaceState(
              null,
              '',
              writeAddress(window.location.search, published, next),
            );
          }}
        />
        <Console
          run={run}
          chrome={chrome}
          velocities={trajectory.facts?.velocities ?? false}
        />
      </div>
    </>
  );
}
