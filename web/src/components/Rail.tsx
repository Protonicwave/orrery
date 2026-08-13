import type { ReactNode } from 'react';
import type { Run } from '../config/run';
import { MACHINE, MEASURED } from '../data/machine';
import type { Diagnostics } from '../diagnostics/series';
import { setDecimal, setScientific } from '../format/number';
import type { InstantSource } from '../state/instant';
import { Diagnostic } from './Diagnostic';
import { Numeric } from './Numeric';
import styles from './Rail.module.css';

export interface RailProps {
  run: Run;
  /** The run's own diagnostics, or null while they are still being read. */
  diagnostics: Diagnostics | null;
  instants: InstantSource;
  /** Whether the console's derived tier is asking for the plots. */
  showDiagnostics: boolean;
  /** A sentence, if the diagnostics could not be read. */
  message: string;
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
 * How each quantity is set. Defined once, outside the component, so that the
 * subscription in each plot is given the same function on every render rather
 * than a new one that would resubscribe it.
 */
const DRIFT = (value: number) => setScientific(value);
const RATIO = (value: number) => setDecimal(value, 3);

/**
 * The data rail: what the run measured, what it is, and what it ran on.
 *
 * Every figure in the first two registers comes from a file this repository
 * produced: the plots and the drift figures out of the run's own diagnostics
 * CSV, the configuration out of the configuration file the run was given. The
 * last two are transcribed from the reports, each with the conditions it was
 * measured under beside it.
 */
export function Rail({
  run,
  diagnostics,
  instants,
  showDiagnostics,
  message,
}: RailProps) {
  const configuration = run.configuration;
  const initial = configuration.initial_conditions ?? {};
  const solver = configuration.solver ?? {};

  const samples = diagnostics?.samples ?? 0;
  const last = samples - 1;

  return (
    // The rail scrolls and holds no control, so it takes a tab stop of its
    // own. Otherwise its lower registers are reachable with a pointer and by
    // nothing else.
    //
    // The linter's rule and the accessibility rule disagree here, and they are
    // arguing about different cases: a tab stop on a static element is
    // confusing, and a scrollable region that only a pointer can scroll is
    // unusable. The rule that a reader cannot work around wins.
    // biome-ignore lint/a11y/noNoninteractiveTabindex: a scrollable region has to be reachable by keyboard
    <aside className={styles.rail} aria-label="Run data" tabIndex={0}>
      {showDiagnostics && (
        <section className={styles.section}>
          <h2 className="label">Diagnostics</h2>

          {diagnostics === null ? (
            <p className={styles.provenance}>
              {message === '' ? 'Reading the run’s diagnostics' : message}
            </p>
          ) : (
            <>
              <Diagnostic
                name="Energy drift"
                values={diagnostics.energyDrift}
                times={diagnostics.time}
                modelTime={run.modelTime}
                instants={instants}
                note={`${samples} samples`}
                symbol="ΔE/E₀"
                format={DRIFT}
              />
              <Diagnostic
                name="Virial ratio"
                values={diagnostics.virialRatio}
                times={diagnostics.time}
                modelTime={run.modelTime}
                instants={instants}
                note="−2T/U"
                symbol="1 in balance"
                format={RATIO}
              />
              <Diagnostic
                name="Angular momentum"
                values={diagnostics.angularMomentum}
                times={diagnostics.time}
                modelTime={run.modelTime}
                instants={instants}
                note="magnitude"
                symbol="|L|"
                format={DRIFT}
              />
              {/* Linear momentum rather than step time, which the diagnostics
                  file does not carry. ADR-0048 gives the reason and this is
                  the better plot in any case: a total momentum is the
                  cancellation of N terms of both signs. */}
              <Diagnostic
                name="Linear momentum"
                values={diagnostics.linearMomentum}
                times={diagnostics.time}
                modelTime={run.modelTime}
                instants={instants}
                note="magnitude"
                symbol="|p|"
                format={DRIFT}
              />
            </>
          )}
        </section>
      )}

      <section className={styles.section}>
        <h2 className="label">Measured</h2>
        <Group title="This run">
          {diagnostics === null || last < 0 ? (
            <Row name="diagnostics">not read</Row>
          ) : (
            <>
              <Row name="energy drift" strong>
                <Numeric
                  value={diagnostics.energyDrift[last] as number}
                  notation="scientific"
                />
              </Row>
              <Row name="virial ratio">
                <Numeric value={diagnostics.virialRatio[0] as number} digits={2} /> →{' '}
                <Numeric value={diagnostics.virialRatio[last] as number} digits={2} />
              </Row>
              <Row name="samples">
                <Numeric value={samples} />
              </Row>
            </>
          )}
        </Group>

        <Group title="The demonstration">
          <Row name="step time">
            <Numeric value={MEASURED.stepTime} digits={1} unit="ms" />
          </Row>
          <Row name="wall clock">
            <Numeric value={MEASURED.wallClock} unit="s" />
          </Row>
        </Group>
        <p className={styles.provenance}>
          The first register is this run’s own diagnostics file. The step time is not in
          it, because how long a step took is a property of the machine rather than of
          the state; it is the README’s demonstration at{' '}
          <Numeric value={MEASURED.count} /> particles over{' '}
          <Numeric value={MEASURED.steps} /> steps. A step time belongs to a particle
          count.
        </p>
      </section>

      <section className={styles.section}>
        <h2 className="label">Configuration</h2>
        <Group title="Initial conditions">
          <Row name="kind" strong>
            {initial.kind}
          </Row>
          {initial.count !== undefined && (
            <Row name="count" strong>
              <Numeric value={Number(initial.count)} />
            </Row>
          )}
          {Object.entries(initial)
            .filter(([key]) => key !== 'kind' && key !== 'count')
            .map(([key, value]) => (
              <Row name={key.replace(/_/g, ' ')} key={key}>
                {value}
              </Row>
            ))}
        </Group>

        <Group title="Solver">
          <Row name="kind" strong>
            {run.solver}
          </Row>
          {solver.softening !== undefined && (
            <Row name="softening">{solver.softening}</Row>
          )}
          {solver.opening_angle !== undefined && (
            <Row name="opening angle">{solver.opening_angle}</Row>
          )}
        </Group>

        <Group title="Integrator">
          <Row name="kind" strong>
            {run.integrator}
          </Row>
          <Row name="seed">{run.seed}</Row>
        </Group>
      </section>

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
            <Numeric value={run.timestep} digits={5} />
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
