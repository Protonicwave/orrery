import type { Run } from '../config/run';
import styles from './Masthead.module.css';

/** The documentation site, which is where the reference material is published. */
const REFERENCE = 'https://protonicwave.github.io/orrery/';

export interface MastheadProps {
  run: Run;
}

/**
 * The instrument's top rule: what this is, where else to go, and what is
 * loaded.
 *
 * The build line states the run's solver, the precision the figures were taken
 * in and the version, in the way a plate is captioned. The state beside it
 * says what the instrument is doing, and it says the truth: a configuration is
 * loaded and nothing is being integrated.
 */
export function Masthead({ run }: MastheadProps) {
  return (
    <header className={styles.masthead}>
      <div className={styles.wordmark}>
        {/* Concentric rings: the instrument the project is named after. */}
        <svg
          className={styles.rings}
          width="16"
          height="16"
          viewBox="0 0 16 16"
          fill="none"
          aria-hidden="true"
        >
          <circle cx="8" cy="8" r="7" strokeWidth="1" />
          <circle className={styles.inner} cx="8" cy="8" r="4.2" strokeWidth="1" />
          <circle className={styles.sun} cx="8" cy="8" r="1.5" />
          <circle className={styles.body} cx="15" cy="8" r="1.1" />
          <circle className={styles.moon} cx="8" cy="3.8" r="1" />
        </svg>
        <b>Orrery</b>
      </div>

      <nav className={styles.nav} aria-label="Sections">
        <a href="./" aria-current="page">
          Instrument
        </a>
        <a href={REFERENCE}>Reference</a>
      </nav>

      <div className={styles.right}>
        <p className={styles.build}>
          {run.solver} · <em>cpu</em> · {run.precision} · v{__ORRERY_VERSION__}
        </p>
        <p className={styles.state}>
          <i aria-hidden="true" /> Loaded
        </p>
      </div>
    </header>
  );
}
