import { useId } from 'react';
import type { Run } from '../config/run';
import { MEASURED } from '../data/machine';
import { decimal, withSign } from '../format/number';
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

export interface ConsoleProps {
  run: Run;
  chrome: Store<ChromeState>;
  /** Whether the trajectory being played carries velocities as well. */
  velocities: boolean;
}

/**
 * What a run of this size would cost, in seconds.
 *
 * The tree solver's cost goes as N log N, and the one measured point the
 * repository has for this configuration is 20.4 ms a step at twenty thousand
 * particles. Scaling that point by N log N and multiplying by the run's steps
 * is an estimate rather than a measurement, which is why the control says
 * "est." and why the measured figure it rests on is shown in the data rail
 * beside the conditions it was taken under.
 */
export function estimateSeconds(count: number, steps: number): number {
  const reference = MEASURED.count * Math.log2(MEASURED.count);
  const work = count * Math.log2(count);
  return (MEASURED.stepTime * (work / reference) * steps) / 1000;
}

function price(count: number, steps: number): string {
  const seconds = estimateSeconds(count, steps);
  const time =
    seconds < 90
      ? `${decimal(seconds)} s`
      : seconds < 5400
        ? `${decimal(seconds / 60, 1)} min`
        : `${decimal(seconds / 3600, 1)} h`;
  return `est. ${time} · ${decimal((seconds * 1000) / steps, 1)} ms/step`;
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
export function Console({ run, chrome, velocities }: ConsoleProps) {
  const state = useStoreState(chrome);
  const id = useId();

  const viewNote = `${id}-view-note`;
  const derivedNote = `${id}-derived-note`;
  const solverNote = `${id}-solver-note`;

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
          max={6.3}
          step={0.01}
          display={<Numeric value={state.requestedCount} />}
          valueText={`${decimal(state.requestedCount)} bodies`}
          disabled
          describedBy={solverNote}
          onChange={(logarithm) =>
            chrome.set({ requestedCount: Math.round(10 ** logarithm / 100) * 100 })
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
          disabled
          describedBy={solverNote}
          onChange={(requestedSoftening) => chrome.set({ requestedSoftening })}
        />
        <Segmented
          name="Integrator"
          dear
          options={INTEGRATORS}
          value={state.requestedIntegrator}
          disabled
          describedBy={solverNote}
          onChange={(requestedIntegrator) => chrome.set({ requestedIntegrator })}
        />

        <p className={styles.note} id={solverNote}>
          A run is integrated by the compute service, which this instrument does not
          reach yet. The estimate below is this machine’s measured step time scaled as
          the tree solver scales, not a promise.
        </p>

        <button
          type="button"
          className={styles.recompute}
          aria-disabled="true"
          aria-describedby={solverNote}
          onClick={(event) => event.preventDefault()}
        >
          <span>Recompute</span>
          <span className={styles.price}>{price(state.requestedCount, run.steps)}</span>
        </button>
      </section>
    </div>
  );
}
