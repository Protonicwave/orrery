import type { ReactNode } from 'react';
import { methodFrom, type Page, pageFrom } from '../method/links';
import styles from './Masthead.module.css';

/** The documentation site, which is where the reference material is published. */
const REFERENCE = 'https://protonicwave.github.io/orrery/';

export interface MastheadProps {
  /** Which page this is, so the navigation marks it and addresses it from here. */
  page: Page;
  /** The caption line: what is loaded and what produced it. */
  build: ReactNode;
  /** What the page is doing, in one word. */
  state: string;
}

/**
 * The top rule of a page: what this is, where else to go, and what is loaded.
 *
 * The build line is captioned the way a plate is captioned, and it says the
 * truth about the page it is on. The instrument states the run's solver and the
 * precision its figures were taken in; the editor states that nothing has been
 * integrated yet, because in the editor nothing has.
 */
export function Masthead({ page, build, state }: MastheadProps) {
  const method = methodFrom(page);

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
        <a
          href={pageFrom(page, 'instrument')}
          aria-current={page === 'instrument' ? 'page' : undefined}
        >
          Instrument
        </a>
        <a
          href={pageFrom(page, 'editor')}
          aria-current={page === 'editor' ? 'page' : undefined}
        >
          Editor
        </a>
        <a href={method.contents}>Method</a>
        <a href={REFERENCE}>Reference</a>
      </nav>

      <div className={styles.right}>
        <p className={styles.build}>{build}</p>
        <p className={styles.state}>
          <i aria-hidden="true" /> {state}
        </p>
      </div>
    </header>
  );
}
