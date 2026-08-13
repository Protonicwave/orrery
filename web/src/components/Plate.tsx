import type { Run } from '../config/run';
import { Numeric } from './Numeric';
import styles from './Plate.module.css';

export interface PlateProps {
  run: Run;
  exposure: number;
}

/**
 * The plate: the surface a run is exposed onto.
 *
 * It carries its own catalogue in the corners, the way an observatory plate
 * does: which run, which configuration, which seed, and the view parameters
 * the exposure was taken under. Everything in those corners is a fact about
 * the loaded configuration rather than a caption written for the picture.
 *
 * The plate is unexposed until a trajectory is loaded, and it says so. An
 * empty black rectangle and a broken renderer look identical, and only one of
 * them should.
 */
export function Plate({ run, exposure }: PlateProps) {
  return (
    <section className={styles.plate} id="plate" aria-labelledby="plate-heading">
      <h2 className="visually-hidden" id="plate-heading">
        The plate
      </h2>

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
            <Numeric value={exposure} notation="signed" />
          </dd>
        </div>
        <div>
          <dt>PROJ</dt> <dd>orthographic</dd>
        </div>
        <div>
          <dt>FRAME</dt> <dd>centre of mass</dd>
        </div>
      </dl>

      <p className={styles.unexposed}>The plate is unexposed</p>
    </section>
  );
}
