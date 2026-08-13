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

const TONE_CURVES: readonly ToneCurve[] = ['linear', 'reinhard', 'aces'];
const OVERLAYS: readonly Overlay[] = ['none', 'octree', 'density'];
const INTEGRATORS: readonly Integrator[] = ['velocity-verlet', 'yoshida4', 'rk4'];

export interface ConsoleProps {
  run: Run;
  chrome: Store<ChromeState>;
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

/** The three tiers, laid out left to right by what operating them costs. */
export function Console({ run, chrome }: ConsoleProps) {
  const state = useStoreState(chrome);
  const describedBy = useId();

  return (
    <div className={styles.console}>
      <section className={styles.tier} aria-labelledby={`${describedBy}-view`}>
        <div className={styles.head}>
          <h2 className="label" id={`${describedBy}-view`}>
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
            onChange={(trails) => chrome.set({ trails })}
          />
          <Toggle
            label="Lab frame"
            pressed={state.labFrame}
            onChange={(labFrame) => chrome.set({ labFrame })}
          />
          <Toggle
            label="Bulge only"
            pressed={state.bulgeOnly}
            onChange={(bulgeOnly) => chrome.set({ bulgeOnly })}
          />
        </Toggles>
      </section>

      <section className={styles.tier} aria-labelledby={`${describedBy}-derived`}>
        <div className={styles.head}>
          <h2 className="label" id={`${describedBy}-derived`}>
            Derived
          </h2>
          <span className={styles.cost}>&lt; 1 s · from trajectory</span>
        </div>

        <Segmented
          name="Overlay"
          options={OVERLAYS}
          value={state.overlay}
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
            onChange={(boundUnbound) => chrome.set({ boundUnbound })}
          />
        </Toggles>
      </section>

      <section
        className={`${styles.tier} ${styles.dear}`}
        aria-labelledby={`${describedBy}-solver`}
      >
        <div className={styles.head}>
          <h2 className="label" id={`${describedBy}-solver`}>
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
          onChange={(requestedSoftening) => chrome.set({ requestedSoftening })}
        />
        <Segmented
          name="Integrator"
          dear
          options={INTEGRATORS}
          value={state.requestedIntegrator}
          onChange={(requestedIntegrator) => chrome.set({ requestedIntegrator })}
        />

        <button
          type="button"
          className={styles.recompute}
          aria-disabled="true"
          aria-describedby={`${describedBy}-needs`}
          onClick={(event) => event.preventDefault()}
        >
          <span>Recompute</span>
          <span className={styles.price}>{price(state.requestedCount, run.steps)}</span>
        </button>
        <span className="visually-hidden" id={`${describedBy}-needs`}>
          A new run is computed by the service, which this instrument does not yet
          reach.
        </span>
      </section>
    </div>
  );
}
