import { useMemo, useState } from 'react';
import styles from './App.module.css';
import { Console } from './components/Console';
import { Masthead } from './components/Masthead';
import { Plate } from './components/Plate';
import { Rail } from './components/Rail';
import { Transport } from './components/Transport';
import { numeric } from './config/parse';
import { collision } from './config/run';
import { createChromeState } from './state/store';
import { useStoreValue } from './state/useStore';

/**
 * The instrument.
 *
 * The run is fixed for now: the collision the repository demonstrates, read
 * out of its own configuration file. Which run is loaded becomes a property of
 * the address when the gallery exists.
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

  // The instant being read. It moves when the transport is scrubbed, which is
  // hundreds of times an hour rather than sixty times a second, so it is
  // chrome state and React may hold it.
  const [time, setTime] = useState(0);
  const exposure = useStoreValue(chrome, (state) => state.exposure);

  return (
    <>
      <a className="skip-link" href="#plate">
        Skip to the instrument
      </a>
      <div className={styles.instrument}>
        <Masthead run={run} />
        <div className={styles.stage}>
          <Plate run={run} exposure={exposure} />
          <Rail run={run} />
        </div>
        <Transport run={run} time={time} onSeek={setTime} />
        <Console run={run} chrome={chrome} />
      </div>
    </>
  );
}
