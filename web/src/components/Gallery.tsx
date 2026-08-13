import type { Run } from '../config/run';
import type { GalleryRun } from '../gallery/runs';
import styles from './Gallery.module.css';
import { Numeric } from './Numeric';

export interface GalleryProps {
  runs: readonly Run[];
  current: GalleryRun;
  /** Told which run was chosen, with the address it was chosen through. */
  onChoose: (run: GalleryRun) => void;
}

/**
 * The published runs, in the order the argument goes.
 *
 * Each is a real link to the address that opens it, so it can be copied,
 * opened in another tab and read by anything that reads links, and each is
 * intercepted so that choosing one does not reload the page and fetch the
 * client again. That is the whole of the routing: an anchor with an href, and
 * a handler that would be an ordinary navigation if it were removed.
 *
 * What a card states is what its configuration says: how many bodies, how many
 * steps and which solver. Not what its diagnostics say, because that would mean
 * fetching three diagnostics files to draw a strip of three links, and the
 * figures a run measured belong in the rail beside the run they were measured
 * from.
 */
export function Gallery({ runs, current, onChoose }: GalleryProps) {
  return (
    <nav className={styles.gallery} aria-label="Published runs">
      <ul className={styles.list}>
        {runs.map((run) => {
          const chosen = run.published.id === current.id;
          return (
            <li key={run.published.id}>
              <a
                className={chosen ? `${styles.run} ${styles.chosen}` : styles.run}
                href={`?run=${run.published.id}`}
                aria-current={chosen ? 'page' : undefined}
                title={run.published.about}
                onClick={(event) => {
                  // A modified click is a request to open the address
                  // somewhere else, and it is the browser's to answer.
                  if (
                    event.metaKey ||
                    event.ctrlKey ||
                    event.shiftKey ||
                    event.altKey ||
                    event.button !== 0
                  ) {
                    return;
                  }
                  event.preventDefault();
                  onChoose(run.published);
                }}
              >
                <span className={styles.title}>{run.published.title}</span>
                <span className={styles.figures}>
                  {run.count !== undefined && (
                    <>
                      <Numeric value={run.count} /> bodies ·{' '}
                    </>
                  )}
                  <Numeric value={run.steps} /> steps · {run.solver}
                </span>
              </a>
            </li>
          );
        })}
      </ul>
    </nav>
  );
}
