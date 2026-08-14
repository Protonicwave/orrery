import { useId } from 'react';
import type { Run } from '../config/run';
import { MEASURED } from '../data/machine';
import { decimal, withSign } from '../format/number';
import type { Job, Reference } from '../service/contract';
import { type ServiceRun, serviceRun } from '../service/request';
import type { ServiceView } from '../service/useService';
import type { BrowserRun } from '../solver/configure';
import type {
  ChromeState,
  Integrator,
  Overlay,
  Store,
  ToneCurve,
} from '../state/store';
import { useStoreState } from '../state/useStore';
import styles from './Console.module.css';
import { Segmented, Slider, Toggle, Toggles } from './Control';
import { Numeric } from './Numeric';

const TONE_CURVES: readonly ToneCurve[] = ['reinhard', 'linear'];
const OVERLAYS: readonly Overlay[] = ['none', 'octree', 'density'];
const INTEGRATORS: readonly Integrator[] = ['velocity-verlet', 'yoshida4', 'rk4'];

/** What the browser run is doing, for the control that starts and stops it. */
export interface BrowserSolver {
  /** True from the moment it is asked for until it is stopped. */
  readonly running: boolean;
  /** What was asked for, once the module has said what it will accept. */
  readonly plan: BrowserRun | null;
  /** Empty unless it failed, and then a sentence saying how. */
  readonly message: string;
  readonly onStart: () => void;
  readonly onStop: () => void;
}

/**
 * The compute service, as the console operates it.
 *
 * What would be submitted is worked out here rather than passed in, because it
 * is a function of the controls in this component and of the ceilings the
 * service published, and both are already here.
 */
export interface ServiceControl {
  readonly view: ServiceView;
  readonly onSubmit: (configuration: string) => void;
  readonly onDismiss: () => void;
}

export interface ConsoleProps {
  run: Run;
  chrome: Store<ChromeState>;
  /** Whether the trajectory being played carries velocities as well. */
  velocities: boolean;
  solver: BrowserSolver;
  service: ServiceControl;
}

/**
 * What a run of this size would cost, in seconds.
 *
 * The tree solver's cost goes as N log N, so the estimate is one measured step
 * time scaled along that curve and multiplied by the steps. Which measured step
 * time is the whole question.
 *
 * The service's own is used when it has one, because it is the machine that
 * would take the run. It is the median over the runs that service has actually
 * completed, so it starts existing after the first one and gets better.
 *
 * Before then the only measurement this repository has is the laptop's: 20.4 ms
 * a step at twenty thousand particles, from the demonstration in README.md.
 * That is a different machine and the estimate says so rather than passing it
 * off, which is why `priced` returns where the figure came from as well as what
 * it is.
 */
export function estimateSeconds(
  count: number,
  steps: number,
  reference: Reference | null,
): number {
  const work = count * Math.log2(count);
  const at = reference === null ? MEASURED.count : reference.particles;
  const stepTime = reference === null ? MEASURED.stepTime : reference.step_ms;
  const measured = at * Math.log2(at);
  if (measured <= 0) return 0;
  return (stepTime * (work / measured) * steps) / 1000;
}

function duration(seconds: number): string {
  if (seconds < 90) return `${decimal(seconds)} s`;
  if (seconds < 5400) return `${decimal(seconds / 60, 1)} min`;
  return `${decimal(seconds / 3600, 1)} h`;
}

/** The estimate, and one clause saying which measurement it rests on. */
function priced(count: number, steps: number, reference: Reference | null): string {
  const seconds = estimateSeconds(count, steps, reference);
  const from =
    reference === null
      ? 'from this project’s laptop'
      : `from ${decimal(reference.jobs)} run${reference.jobs === 1 ? '' : 's'} here`;
  return `est. ${duration(seconds)} · ${from}`;
}

/**
 * The one run this instrument can compute for itself.
 *
 * It sits at the foot of the solver tier because it is a new run rather than
 * something derived from the one on the plate, and it is separated from the
 * controls above it because those describe a run the compute service would
 * take and this one describes a run this tab takes.
 *
 * What the run turns out to be is not here but on the plate, in the catalogue
 * beside the exposure: the count, the backend, the step time and the energy
 * drift are the conditions the picture was taken under, and a plate is where
 * those are written. A browser stepping a few thousand particles on one thread
 * with a scalar kernel must never be read as the hardware the performance
 * reports were taken on, and that is prevented by saying so where the picture
 * is rather than where the button is.
 */
function BrowserRunControl({
  run,
  solver,
  noteId,
}: {
  run: Run;
  solver: BrowserSolver;
  noteId: string;
}) {
  const { plan, running, message } = solver;

  return (
    <div className={styles.here}>
      <button
        type="button"
        className={styles.recompute}
        aria-describedby={noteId}
        onClick={running ? solver.onStop : solver.onStart}
      >
        <span>{running ? 'Stop the browser run' : 'Run it in this browser'}</span>
        <span className={styles.price}>
          {plan === null
            ? 'WebAssembly · same solver'
            : `${decimal(plan.count)} bodies · ${decimal(plan.steps)} steps`}
        </span>
      </button>

      <p className={styles.note} id={noteId}>
        {message !== ''
          ? message
          : plan === null
            ? 'The same C++ as the native binary, compiled to WebAssembly and stepped in a Worker, on one thread and with the scalar kernel. What it shows is the physics rather than the performance.'
            : `The same C++ as the native binary, compiled to WebAssembly and stepped in a Worker, on one thread and with the scalar kernel. ${run.published.title} is cut to ${decimal(plan.count)} bodies and the first ${decimal(plan.steps)} steps to fit in a tab. The measured figures in the data rail come from the published run and the machine named beside them.`}
      </p>
    </div>
  );
}

/**
 * How a submitted run is going, in one line under the button that sent it.
 *
 * Everything shown is something the service measured: the position in the
 * queue, the step the run has reached, the step time the worker timed and the
 * energy drift out of the diagnostics file the run itself is writing. Nothing
 * is interpolated between reports, because a bar that moves smoothly through a
 * run that has stalled is a bar that lies.
 */
function Submitted({ job, onDismiss }: { job: Job; onDismiss: () => void }) {
  const fraction = job.steps === 0 ? 0 : job.progress.step / job.steps;

  const state =
    job.state === 'queued'
      ? job.position === null || job.position === 0
        ? 'queued, next to run'
        : `queued, ${decimal(job.position)} ahead`
      : job.state === 'running'
        ? `running · ${decimal(fraction * 100)}%`
        : job.state === 'done'
          ? 'complete'
          : 'failed';

  return (
    <div className={styles.here}>
      <p className={styles.note}>
        <strong>{state}</strong>
        {job.state === 'running' && job.progress.step_ms !== null && (
          <>
            {' · '}
            <Numeric value={job.progress.step_ms} digits={1} unit="ms" /> a step
          </>
        )}
        {job.progress.energy_drift !== null && (
          <>
            {' · dE/E '}
            <Numeric
              value={job.progress.energy_drift}
              notation="scientific"
              digits={2}
            />
          </>
        )}
        {job.attempts > 1 && job.state !== 'failed' && (
          <>
            {' · '}the worker that first took this run stopped answering, so it was
            given to another
          </>
        )}
      </p>

      {job.state === 'failed' && <p className={styles.note}>{job.error}</p>}

      <button type="button" className={styles.recompute} onClick={onDismiss}>
        <span>
          {job.state === 'done' || job.state === 'failed'
            ? 'Back to the published run'
            : 'Stop watching this run'}
        </span>
        <span className={styles.price}>
          {decimal(job.particles)} bodies · {decimal(job.steps)} steps
        </span>
      </button>
    </div>
  );
}

/**
 * The three tiers, laid out left to right by what operating them costs.
 *
 * Every control that cannot act is drawn back rather than removed, and the
 * foot of its tier says what it would need. Those notes are the most useful
 * writing in the interface: between them they say exactly what a trajectory
 * is. A trajectory holds positions and masses, at a stride, and nothing else.
 * It does not hold which component a particle was sampled into, so there is no
 * bulge to show on its own and no pair for a frame to rotate with; it does not
 * hold velocities unless it was asked to, so nothing can be called bound or
 * unbound; and it does not hold the tree the solver built, so there is no
 * octree to draw over it.
 */
export function Console({ run, chrome, velocities, solver, service }: ConsoleProps) {
  const state = useStoreState(chrome);
  const id = useId();

  const viewNote = `${id}-view-note`;
  const derivedNote = `${id}-derived-note`;
  const solverNote = `${id}-solver-note`;
  const hereNote = `${id}-here-note`;

  const { view } = service;
  const limits = view.capabilities?.limits ?? null;

  // What the button would send. Null until the service has said what it will
  // take, because a submission built against guessed ceilings is one the
  // service would refuse for a reason the page could have known.
  const plan: ServiceRun | null =
    limits === null
      ? null
      : serviceRun(
          run,
          {
            count: state.requestedCount,
            softening: state.requestedSoftening,
            integrator: state.requestedIntegrator,
          },
          limits,
        );

  // The most this service will take, or what the run already is when that is
  // not known. A slider that offered more than the service accepts would be a
  // control whose only outcome was a refusal.
  const ceiling = limits?.max_particles ?? state.requestedCount;

  // Why the tier cannot be operated, in one sentence, or empty when it can.
  // Three different failures with the same answer for the reader, which is that
  // the published runs are unaffected and are worth looking at.
  const refusal =
    view.unreachable !== ''
      ? `${view.unreachable}. The published runs below are unaffected, and every figure in this instrument comes from one of them.`
      : view.capabilities === null
        ? ''
        : view.capabilities.workers === 0
          ? 'No worker is available to take a run at the moment. The published runs below are unaffected.'
          : view.capabilities.queued >= view.capabilities.limits.max_queue
            ? `The queue is full at ${decimal(view.capabilities.queued)} runs. The published runs below are unaffected.`
            : '';

  // Adjustable and submittable are not the same question. The controls stay
  // live while a submission is in flight, so that somebody can see what they
  // asked for; what is refused is sending a second one.
  const adjustable = refusal === '' && limits !== null;
  const submittable = adjustable && !view.busy && plan !== null;

  return (
    <div className={styles.console}>
      <section className={styles.tier} aria-labelledby={`${id}-view`}>
        <div className={styles.head}>
          <h2 className="label" id={`${id}-view`}>
            View
          </h2>
          <span className={styles.cost}>instantaneous</span>
        </div>

        <Slider
          name="Exposure"
          value={state.exposure}
          min={-2}
          max={4}
          step={0.05}
          display={<Numeric value={state.exposure} notation="signed" />}
          valueText={`${withSign(state.exposure)} stops`}
          onChange={(exposure) => chrome.set({ exposure })}
        />
        <Slider
          name="Sprite radius"
          value={state.spriteRadius}
          min={0.5}
          max={3}
          step={0.05}
          display={<Numeric value={state.spriteRadius} digits={2} />}
          valueText={decimal(state.spriteRadius, 2)}
          onChange={(spriteRadius) => chrome.set({ spriteRadius })}
        />
        <Segmented
          name="Tone curve"
          options={TONE_CURVES}
          value={state.toneCurve}
          onChange={(toneCurve) => chrome.set({ toneCurve })}
        />

        <Toggles>
          <Toggle
            label="Trails"
            pressed={state.trails}
            disabled
            describedBy={viewNote}
            onChange={(trails) => chrome.set({ trails })}
          />
          <Toggle
            label="Lab frame"
            pressed={state.labFrame}
            disabled
            describedBy={viewNote}
            onChange={(labFrame) => chrome.set({ labFrame })}
          />
          <Toggle
            label="Bulge only"
            pressed={state.bulgeOnly}
            disabled
            describedBy={viewNote}
            onChange={(bulgeOnly) => chrome.set({ bulgeOnly })}
          />
        </Toggles>

        <p className={styles.note} id={viewNote}>
          Trails need the renderer to accumulate more than the one frame it presents.
          The other two need what a trajectory does not carry: every published run is
          already in the centre-of-mass frame, which the linear momentum plot shows, and
          a frame holds positions rather than which component a particle was sampled
          into.
        </p>
      </section>

      <section className={styles.tier} aria-labelledby={`${id}-derived`}>
        <div className={styles.head}>
          <h2 className="label" id={`${id}-derived`}>
            Derived
          </h2>
          <span className={styles.cost}>&lt; 1 s · from trajectory</span>
        </div>

        <Segmented
          name="Overlay"
          options={OVERLAYS}
          value={state.overlay}
          disabled
          describedBy={derivedNote}
          onChange={(overlay) => chrome.set({ overlay })}
        />
        <Slider
          name="Rotating frame"
          value={state.rotatingFrame}
          min={0}
          max={1}
          step={0.01}
          display={<Numeric value={state.rotatingFrame} digits={2} />}
          valueText={decimal(state.rotatingFrame, 2)}
          disabled
          describedBy={derivedNote}
          onChange={(rotatingFrame) => chrome.set({ rotatingFrame })}
        />
        <Slider
          name="Orbit trace"
          value={state.orbitTrace}
          min={0}
          max={400}
          step={10}
          display={<Numeric value={state.orbitTrace} unit="steps" />}
          valueText={`${decimal(state.orbitTrace)} steps`}
          disabled
          describedBy={derivedNote}
          onChange={(orbitTrace) => chrome.set({ orbitTrace })}
        />

        <Toggles>
          <Toggle
            label="Diagnostics"
            pressed={state.diagnostics}
            onChange={(diagnostics) => chrome.set({ diagnostics })}
          />
          <Toggle
            label="Radial profile"
            pressed={state.radialProfile}
            onChange={(radialProfile) => chrome.set({ radialProfile })}
          />
          <Toggle
            label="Bound / unbound"
            pressed={state.boundUnbound}
            disabled={!velocities}
            describedBy={derivedNote}
            onChange={(boundUnbound) => chrome.set({ boundUnbound })}
          />
        </Toggles>

        <p className={styles.note} id={derivedNote}>
          An octree overlay needs the tree the solver built and a rotating frame needs
          to know which galaxy each particle came from, neither of which a trajectory
          records. An orbit trace needs the renderer to draw several frames into one
          picture. Bound and unbound need velocities, and a published run is written
          without them.
        </p>
      </section>

      <section
        className={`${styles.tier} ${styles.dear}`}
        aria-labelledby={`${id}-solver`}
      >
        <div className={styles.head}>
          <h2 className="label" id={`${id}-solver`}>
            Solver
          </h2>
          <span className={styles.cost}>requires a new run</span>
        </div>

        <Slider
          name="Bodies"
          dear
          value={Math.log10(state.requestedCount)}
          min={3.7}
          max={Math.max(3.7, Math.log10(ceiling))}
          step={0.01}
          display={<Numeric value={state.requestedCount} />}
          valueText={`${decimal(state.requestedCount)} bodies`}
          disabled={!adjustable || run.count === undefined}
          describedBy={solverNote}
          onChange={(logarithm) =>
            chrome.set({
              requestedCount: Math.min(
                ceiling,
                Math.round(10 ** logarithm / 100) * 100,
              ),
            })
          }
        />
        <Slider
          name="Softening"
          dear
          value={state.requestedSoftening}
          min={0.02}
          max={0.4}
          step={0.005}
          display={<Numeric value={state.requestedSoftening} digits={3} />}
          valueText={decimal(state.requestedSoftening, 3)}
          disabled={!adjustable}
          describedBy={solverNote}
          onChange={(requestedSoftening) => chrome.set({ requestedSoftening })}
        />
        <Segmented
          name="Integrator"
          dear
          options={INTEGRATORS}
          value={state.requestedIntegrator}
          disabled={!adjustable}
          describedBy={solverNote}
          onChange={(requestedIntegrator) => chrome.set({ requestedIntegrator })}
        />

        <p className={styles.note} id={solverNote}>
          {refusal !== ''
            ? refusal
            : plan === null
              ? 'A run is integrated by the compute service, which this instrument is asking about.'
              : `The service runs the same binary this repository builds, on its own hardware. ${
                  plan.reduced
                    ? `This scenario is cut to ${decimal(plan.count)} bodies and ${decimal(plan.steps)} steps to fit what it will take. `
                    : ''
                }${
                  run.count === undefined
                    ? 'This scenario is two bodies, so there is no count to choose. '
                    : ''
                }The estimate is a measured step time scaled as the tree solver scales, not a promise.`}
        </p>

        {view.job !== null ? (
          <Submitted job={view.job} onDismiss={service.onDismiss} />
        ) : (
          <>
            <button
              type="button"
              className={styles.recompute}
              aria-disabled={submittable ? undefined : 'true'}
              aria-describedby={solverNote}
              onClick={(event) => {
                if (!submittable || plan === null) {
                  event.preventDefault();
                  return;
                }
                service.onSubmit(plan.text);
              }}
            >
              <span>{view.busy ? 'Submitting' : 'Recompute'}</span>
              <span className={styles.price}>
                {priced(
                  plan?.count ?? state.requestedCount,
                  plan?.steps ?? run.steps,
                  view.capabilities?.reference ?? null,
                )}
              </span>
            </button>

            {view.problems.length > 0 && (
              <ul className={styles.note}>
                {view.problems.map((problem) => (
                  <li key={`${problem.setting}:${problem.complaint}`}>
                    <strong>{problem.setting}</strong> {problem.complaint}
                  </li>
                ))}
              </ul>
            )}
          </>
        )}

        <BrowserRunControl run={run} solver={solver} noteId={hereNote} />
      </section>
    </div>
  );
}
