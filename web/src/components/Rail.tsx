import type { ReactNode } from 'react';
import type { Run } from '../config/run';
import { MACHINE, MEASURED } from '../data/machine';
import { Numeric } from './Numeric';
import styles from './Rail.module.css';

export interface RailProps {
  run: Run;
}

/** One row of a register: a name, and a value that may carry a unit. */
function Row({
  name,
  children,
  strong = false,
}: {
  name: string;
  children: ReactNode;
  strong?: boolean;
}) {
  return (
    <>
      <dt>{name}</dt>
      <dd>{strong ? <b>{children}</b> : children}</dd>
    </>
  );
}

/**
 * A group of settings under a heading.
 *
 * The heading sits outside the list rather than inside it as a row, because a
 * definition list holds terms and their definitions and a group name is
 * neither. Each group is its own list for the same reason.
 */
function Group({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className={styles.group}>
      <h3>{title}</h3>
      <dl className={styles.table}>{children}</dl>
    </div>
  );
}

/**
 * The data rail: what the run is, what it measured, and on what.
 *
 * Every figure in the first register is parsed out of the configuration file
 * itself, so the rail cannot state a setting the run was not given. The second
 * and third are transcribed from the reports, each with the conditions it was
 * measured under beside it.
 */
export function Rail({ run }: RailProps) {
  const configuration = run.configuration;
  const initial = configuration.initial_conditions ?? {};
  const solver = configuration.solver ?? {};

  return (
    // The rail scrolls and holds no control, so it takes a tab stop of its
    // own. Otherwise its lower registers are reachable with a pointer and by
    // nothing else.
    <aside className={styles.rail} aria-label="Run data" tabIndex={0}>
      <section className={styles.section}>
        <h2 className="label">Run</h2>
        <dl className={styles.table}>
          <Row name="model time" strong>
            <Numeric value={run.modelTime} digits={3} />
          </Row>
          <Row name="steps" strong>
            <Numeric value={run.steps} />
          </Row>
          <Row name="timestep">
            <Numeric value={run.timestep} digits={3} />
          </Row>
          <Row name="trajectory frames">
            <Numeric value={run.frames} />
          </Row>
          <Row name="diagnostics samples">
            <Numeric value={run.samples} />
          </Row>
        </dl>
      </section>

      <section className={styles.section}>
        <h2 className="label">Configuration</h2>
        <Group title="Initial conditions">
          <Row name="kind" strong>
            {initial.kind}
          </Row>
          <Row name="count" strong>
            <Numeric value={Number(initial.count)} />
          </Row>
          <Row name="total mass">{initial.total_mass}</Row>
          <Row name="mass ratio">{initial.mass_ratio}</Row>
          <Row name="scale length">{initial.scale_length}</Row>
          <Row name="scale height">{initial.scale_height}</Row>
          <Row name="bulge fraction">{initial.bulge_fraction}</Row>
          <Row name="bulge radius">{initial.bulge_radius}</Row>
          <Row name="inclination">
            {initial.inclination} / {initial.secondary_inclination}
          </Row>
          <Row name="separation">{initial.separation}</Row>
          <Row name="impact parameter">{initial.impact_parameter}</Row>
          <Row name="approach speed">{initial.approach_speed}</Row>
        </Group>

        <Group title="Solver">
          <Row name="kind" strong>
            {solver.kind}
          </Row>
          <Row name="softening">{solver.softening}</Row>
          <Row name="opening angle">{solver.opening_angle}</Row>
        </Group>

        <Group title="Integrator">
          <Row name="kind" strong>
            {run.integrator}
          </Row>
          <Row name="seed">{run.seed}</Row>
        </Group>
      </section>

      <section className={styles.section}>
        <h2 className="label">Measured</h2>
        <dl className={styles.table}>
          <Row name="energy drift" strong>
            <Numeric value={MEASURED.energyDrift} notation="scientific" />
          </Row>
          <Row name="virial ratio">
            <Numeric value={MEASURED.virialStart} digits={2} /> →{' '}
            <Numeric value={MEASURED.virialEnd} digits={2} />
          </Row>
          <Row name="step time">
            <Numeric value={MEASURED.stepTime} digits={1} unit="ms" />
          </Row>
          <Row name="wall clock">
            <Numeric value={MEASURED.wallClock} unit="s" />
          </Row>
        </dl>
        <p className={styles.provenance}>
          Taken at <Numeric value={MEASURED.count} /> particles over{' '}
          <Numeric value={MEASURED.steps} /> steps, which is the demonstration in the
          repository's README. A step time belongs to a particle count.
        </p>
      </section>

      <section className={styles.section}>
        <h2 className="label">Machine</h2>
        <dl className={styles.table}>
          {MACHINE.map((fact) => (
            <Row name={fact.name} key={fact.name}>
              {fact.unit === undefined ? (
                fact.value
              ) : (
                <>
                  {fact.value}
                  <span className="unit"> {fact.unit}</span>
                </>
              )}
            </Row>
          ))}
        </dl>
      </section>
    </aside>
  );
}
