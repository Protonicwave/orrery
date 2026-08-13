import type { ReactNode } from 'react';
import type { Run } from '../config/run';
import { MACHINE, MEASURED } from '../data/machine';
import type { Diagnostics } from '../diagnostics/series';
import { setDecimal, setScientific } from '../format/number';
import type { InstantSource } from '../state/instant';
import type { Trajectory } from '../trajectory/client';
import { Diagnostic } from './Diagnostic';
import { Numeric } from './Numeric';
import { Profile } from './Profile';
import styles from './Rail.module.css';

export interface RailProps {
  run: Run;
  /** The run's own diagnostics, or null while they are still being read. */
  diagnostics: Diagnostics | null;
  instants: InstantSource;
  /** The trajectory being played, for what the client derives from it. */
  trajectory: Trajectory;
  /** Whether the console's derived tier is asking for the plots. */
  showDiagnostics: boolean;
  /** Whether it is asking for the radial profile, which is derived here. */
  showProfile: boolean;
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
function Group({
  title,
  children,
  arriving = false,
}: {
  title: string;
  children: ReactNode;
  /** Whether this group's values are read from a file over the network. */
  arriving?: boolean;
}) {
  return (
    <div className={styles.group}>
      <h3>{title}</h3>
      <dl className={arriving ? `${styles.table} ${styles.arriving}` : styles.table}>
        {children}
      </dl>
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
 * The plots, and what each one is.
 *
 * A list rather than four written out, so that a plot which has no samples yet
 * is drawn as an empty box of exactly the height it will occupy when the file
 * arrives. Nothing on the rail may move when the network answers: the budget is
 * a cumulative layout shift of zero, and a register that grows by four plots
 * two seconds after the page has been read is the largest shift the interface
 * could possibly make.
 */
const NOTHING = new Float64Array(0);

/** What a register holds before the file it reads from has arrived. */
const WAITING = <span className={styles.waiting}>reading</span>;

const PLOTS = [
  {
    key: 'energy',
    name: 'Energy drift',
    column: 'energyDrift',
    note: 'against the first sample',
    symbol: 'ΔE/E₀',
    format: DRIFT,
  },
  {
    key: 'virial',
    name: 'Virial ratio',
    column: 'virialRatio',
    note: 'one in balance',
    symbol: '−2T/U',
    format: RATIO,
  },
  {
    key: 'angular',
    name: 'Angular momentum',
    column: 'angularMomentum',
    note: 'magnitude',
    symbol: '|L|',
    format: DRIFT,
  },
  // Linear momentum rather than step time, which the diagnostics file does not
  // carry. ADR-0048 gives the reason, and it is the better plot in any case: a
  // total momentum is the cancellation of N terms of both signs, so staying at
  // round-off is the strongest conservation statement a run makes.
  {
    key: 'linear',
    name: 'Linear momentum',
    column: 'linearMomentum',
    note: 'magnitude',
    symbol: '|p|',
    format: DRIFT,
  },
] as const;

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
  trajectory,
  showDiagnostics,
  showProfile,
  message,
}: RailProps) {
  const configuration = run.configuration;
  const initial = configuration.initial_conditions ?? {};
  const solver = configuration.solver ?? {};

  const samples = diagnostics?.samples ?? 0;
  const last = samples - 1;
  // A file with no rows in it is not a file that has been read.
  const read = last < 0 ? null : diagnostics;

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

          {PLOTS.map((plot) => (
            <Diagnostic
              key={plot.key}
              name={plot.name}
              values={diagnostics?.[plot.column] ?? NOTHING}
              times={diagnostics?.time ?? NOTHING}
              modelTime={run.modelTime}
              instants={instants}
              note={plot.note}
              symbol={plot.symbol}
              format={plot.format}
            />
          ))}

          {message !== '' && <p className={styles.provenance}>{message}</p>}
        </section>
      )}

      {showProfile && (
        <section className={styles.section}>
          <h2 className="label">Derived</h2>
          <Profile trajectory={trajectory} instants={instants} />
          <p className={styles.provenance}>
            Worked out in the browser from the frame on the plate and the masses in the
            trajectory’s header, rather than read from a file the run wrote. It is the
            instant, where the plots above it are the run.
          </p>
        </section>
      )}

      <section className={styles.section}>
        <h2 className="label">Measured</h2>
        {/* Three rows whether or not the file has arrived, for the same reason
            the plots are drawn empty: a register that grows when the network
            answers moves everything under it. */}
        <Group title="This run" arriving>
          <Row name="energy drift" strong>
            {read === null ? (
              WAITING
            ) : (
              <Numeric value={read.energyDrift[last] as number} notation="scientific" />
            )}
          </Row>
          <Row name="virial ratio">
            {read === null ? (
              WAITING
            ) : (
              <>
                <Numeric value={read.virialRatio[0] as number} digits={2} /> →{' '}
                <Numeric value={read.virialRatio[last] as number} digits={2} />
              </>
            )}
          </Row>
          <Row name="samples">
            {read === null ? WAITING : <Numeric value={samples} />}
          </Row>
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
