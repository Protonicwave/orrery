import { useEffect, useMemo, useRef, useState } from 'react';
import styles from './App.module.css';
import { Console } from './components/Console';
import { Masthead } from './components/Masthead';
import { Plate } from './components/Plate';
import { Rail } from './components/Rail';
import { Transport } from './components/Transport';
import { numeric } from './config/parse';
import { collision } from './config/run';
import { GALLERY, trajectoryUrl } from './gallery/runs';
import type { Instrument } from './render/instrument';
import { createChromeState } from './state/store';
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

  // The instant being read. Written ten times a second by the render loop
  // rather than sixty, which is the rate a clock can be read at and the reason
  // a rendered instant never reaches React on a frame.
  const [time, setTime] = useState(0);
  const playing = useStoreValue(chrome, (state) => state.playing);

  // The transport moves the render loop, which owns where playback is. The
  // reference is filled in by the plate once a backend has started, and stays
  // null if none does, which is what makes the transport a control that says
  // it can do nothing rather than one that appears to work.
  const instrument = useRef<Instrument | null>(null);

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
            chrome={chrome}
            instrumentRef={instrument}
            onSample={setTime}
          />
          <Rail run={run} />
        </div>
        <Transport
          run={run}
          time={time}
          playing={playing}
          onPlayingChange={(next) => chrome.set({ playing: next })}
          onSeek={(next) => {
            setTime(next);
            instrument.current?.seekTime(next);
          }}
        />
        <Console run={run} chrome={chrome} />
      </div>
    </>
  );
}
