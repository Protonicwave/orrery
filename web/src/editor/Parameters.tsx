import { useId } from 'react';
import { Field, Segmented, Slider } from '../components/Control';
import { Numeric } from '../components/Numeric';
import { decimal } from '../format/number';
import {
  type Design,
  type DesignKind,
  type IntegratorKind,
  isGalaxy,
  PRESETS,
  type SolverKind,
  withKind,
} from './design';
import styles from './Parameters.module.css';

const SOLVERS: readonly SolverKind[] = ['direct', 'barnes-hut'];
const INTEGRATORS: readonly IntegratorKind[] = ['velocity-verlet', 'yoshida4', 'rk4'];

/** A right angle in radians, which is where a disc turns over. */
const RIGHT_ANGLE = Math.PI / 2;

function degrees(radians: number): number {
  return (radians * 180) / Math.PI;
}

export interface ParametersProps {
  design: Design;
  onChange: (design: Design) => void;
}

/**
 * An angle, set in radians and read in degrees.
 *
 * The configuration states angles in radians because the mathematics does, and
 * a drawing is dimensioned in degrees because a person reading a drawing thinks
 * in them. Both are true of the same number, so the conversion happens here,
 * where it is displayed, and the design holds only the one the file states.
 */
function Angle({
  name,
  value,
  onChange,
}: {
  name: string;
  value: number;
  onChange: (value: number) => void;
}) {
  return (
    <Slider
      name={name}
      value={value}
      min={0}
      max={Math.PI}
      step={0.01}
      display={<Numeric value={degrees(value)} digits={1} unit="°" />}
      valueText={`${decimal(degrees(value), 1)} degrees, ${value >= RIGHT_ANGLE ? 'retrograde' : 'prograde'}`}
      onChange={onChange}
    />
  );
}

/**
 * The console: everything the configuration lets a person decide.
 *
 * Three tiers, as the instrument has three, and ordered the same way: what the
 * thing is, what shape it is in, and how it will be integrated. Every control
 * sets a setting the format has, under the name the format gives it, so that the
 * console and the file are the same document read two ways.
 */
export function Parameters({ design, onChange }: ParametersProps) {
  const id = useId();
  const set = (patch: Partial<Design>): void => {
    onChange({ ...design, ...patch });
  };

  const galaxy = isGalaxy(design.kind);
  const collision = design.kind === 'galaxy-collision';

  return (
    <div className={styles.console}>
      <section className={styles.tier} aria-labelledby={`${id}-scenario`}>
        <div className={styles.head}>
          <h2 className="label" id={`${id}-scenario`}>
            Scenario
          </h2>
          <span className={styles.cost}>four, as a path</span>
        </div>

        <Segmented
          name="Preset"
          options={PRESETS.map((entry) => entry.id)}
          value={design.kind}
          onChange={(kind: DesignKind) => {
            onChange(withKind(design, kind));
          }}
        />

        {design.kind !== 'kepler' && (
          <>
            <Slider
              name="Particles"
              value={Math.log10(Math.max(design.count, 2))}
              min={Math.log10(64)}
              max={6}
              step={0.01}
              display={<Numeric value={design.count} />}
              valueText={`${decimal(design.count)} particles`}
              onChange={(logarithm) => {
                set({ count: Math.max(2, Math.round(10 ** logarithm / 2) * 2) });
              }}
            />
            <Slider
              name="Total mass"
              value={design.totalMass}
              min={0.1}
              max={10}
              step={0.05}
              display={<Numeric value={design.totalMass} digits={2} />}
              valueText={decimal(design.totalMass, 2)}
              onChange={(totalMass) => {
                set({ totalMass });
              }}
            />
          </>
        )}

        {design.kind === 'plummer' && (
          <Slider
            name="Scale radius"
            value={design.scaleRadius}
            min={0}
            max={2}
            step={0.01}
            display={
              design.scaleRadius > 0 ? (
                <Numeric value={design.scaleRadius} digits={2} />
              ) : (
                <span className={styles.default}>3π/16</span>
              )
            }
            valueText={
              design.scaleRadius > 0
                ? decimal(design.scaleRadius, 2)
                : 'the standard radius, three pi over sixteen'
            }
            onChange={(scaleRadius) => {
              set({ scaleRadius });
            }}
          />
        )}

        {design.kind === 'kepler' && (
          <>
            <Slider
              name="Primary mass"
              value={design.primaryMass}
              min={0.1}
              max={10}
              step={0.05}
              display={<Numeric value={design.primaryMass} digits={2} />}
              valueText={decimal(design.primaryMass, 2)}
              onChange={(primaryMass) => {
                set({ primaryMass });
              }}
            />
            <Slider
              name="Secondary mass"
              value={design.secondaryMass}
              min={0.1}
              max={10}
              step={0.05}
              display={<Numeric value={design.secondaryMass} digits={2} />}
              valueText={decimal(design.secondaryMass, 2)}
              onChange={(secondaryMass) => {
                set({ secondaryMass });
              }}
            />
            <Slider
              name="Semi-major axis"
              value={design.semiMajorAxis}
              min={0.1}
              max={10}
              step={0.05}
              display={<Numeric value={design.semiMajorAxis} digits={2} />}
              valueText={decimal(design.semiMajorAxis, 2)}
              onChange={(semiMajorAxis) => {
                set({ semiMajorAxis });
              }}
            />
            <Slider
              name="Eccentricity"
              value={design.eccentricity}
              min={0}
              max={0.95}
              step={0.01}
              display={<Numeric value={design.eccentricity} digits={2} />}
              valueText={decimal(design.eccentricity, 2)}
              onChange={(eccentricity) => {
                set({ eccentricity });
              }}
            />
          </>
        )}
      </section>

      <section className={styles.tier} aria-labelledby={`${id}-shape`}>
        <div className={styles.head}>
          <h2 className="label" id={`${id}-shape`}>
            {collision ? 'Galaxies and encounter' : 'Shape'}
          </h2>
          <span className={styles.cost}>drawn as you set it</span>
        </div>

        {galaxy ? (
          <>
            <Slider
              name="Bulge share"
              value={design.bulgeFraction}
              min={0}
              max={0.6}
              step={0.01}
              display={
                <Numeric value={design.bulgeFraction * 100} digits={0} unit="%" />
              }
              valueText={`${decimal(design.bulgeFraction * 100)} per cent`}
              onChange={(bulgeFraction) => {
                set({ bulgeFraction });
              }}
            />
            <Slider
              name="Scale length"
              value={design.scaleLength}
              min={0.2}
              max={4}
              step={0.05}
              display={<Numeric value={design.scaleLength} digits={2} />}
              valueText={decimal(design.scaleLength, 2)}
              onChange={(scaleLength) => {
                set({ scaleLength });
              }}
            />
            <Slider
              name="Scale height"
              value={design.scaleHeight}
              min={0.01}
              max={0.5}
              step={0.005}
              display={<Numeric value={design.scaleHeight} digits={3} />}
              valueText={decimal(design.scaleHeight, 3)}
              onChange={(scaleHeight) => {
                set({ scaleHeight });
              }}
            />
            <Slider
              name="Bulge radius"
              value={design.bulgeRadius}
              min={0.02}
              max={1}
              step={0.01}
              display={<Numeric value={design.bulgeRadius} digits={2} />}
              valueText={decimal(design.bulgeRadius, 2)}
              onChange={(bulgeRadius) => {
                set({ bulgeRadius });
              }}
            />
            <Angle
              name={collision ? 'Primary tilt' : 'Inclination'}
              value={design.inclination}
              onChange={(inclination) => {
                set({ inclination });
              }}
            />
          </>
        ) : (
          <p className={styles.note}>
            {design.kind === 'kepler'
              ? 'Two point masses have no shape to set. What there is to decide about them is above: the two masses, the size of the orbit and how eccentric it is.'
              : 'A Plummer sphere is described by its scale radius alone, which is above. It is spherical, its velocities are isotropic and it has no axis, which is exactly why it is the configuration held to a conservation figure.'}
          </p>
        )}

        {collision && (
          <>
            <Angle
              name="Secondary tilt"
              value={design.secondaryInclination}
              onChange={(secondaryInclination) => {
                set({ secondaryInclination });
              }}
            />
            <Slider
              name="Mass ratio"
              value={design.massRatio}
              min={0.05}
              max={1}
              step={0.01}
              display={<Numeric value={design.massRatio} digits={2} />}
              valueText={decimal(design.massRatio, 2)}
              onChange={(massRatio) => {
                set({ massRatio });
              }}
            />
            <Slider
              name="Separation"
              value={design.separation}
              min={2}
              max={60}
              step={0.5}
              display={<Numeric value={design.separation} digits={1} />}
              valueText={decimal(design.separation, 1)}
              onChange={(separation) => {
                set({ separation });
              }}
            />
            <Slider
              name="Impact parameter"
              value={design.impactParameter}
              min={0}
              max={20}
              step={0.1}
              display={<Numeric value={design.impactParameter} digits={1} />}
              valueText={decimal(design.impactParameter, 1)}
              onChange={(impactParameter) => {
                set({ impactParameter });
              }}
            />
            <Slider
              name="Approach speed"
              value={design.approachSpeed}
              min={0}
              max={1.6}
              step={0.01}
              display={<Numeric value={design.approachSpeed} digits={2} />}
              valueText={`${decimal(design.approachSpeed, 2)} of the escape speed`}
              onChange={(approachSpeed) => {
                set({ approachSpeed });
              }}
            />
            <p className={styles.note}>
              The approach speed is a multiple of the escape speed at the initial
              separation, so one is exactly parabolic: below it the pair is bound and
              merges, above it they pass once and separate for ever.
            </p>
          </>
        )}
      </section>

      <section className={styles.tier} aria-labelledby={`${id}-run`}>
        <div className={styles.head}>
          <h2 className="label" id={`${id}-run`}>
            Run
          </h2>
          <span className={styles.cost}>what the file asks for</span>
        </div>

        <Field
          name="Timestep"
          value={design.timestep}
          min={0}
          step={0.001}
          onChange={(timestep) => {
            set({ timestep });
          }}
        />
        <Field
          name="Steps"
          value={design.steps}
          min={1}
          step={1000}
          onChange={(steps) => {
            set({ steps: Math.max(1, Math.round(steps)) });
          }}
        />
        <Field
          name="Seed"
          value={design.seed}
          min={0}
          step={1}
          onChange={(seed) => {
            set({ seed: Math.max(0, Math.round(seed)) });
          }}
        />
        <Slider
          name="Softening"
          value={design.softening}
          min={0}
          max={0.5}
          step={0.005}
          display={<Numeric value={design.softening} digits={3} />}
          valueText={decimal(design.softening, 3)}
          onChange={(softening) => {
            set({ softening });
          }}
        />
        <Segmented
          name="Solver"
          options={SOLVERS}
          value={design.solver}
          onChange={(solver) => {
            set({ solver });
          }}
        />
        <Segmented
          name="Integrator"
          options={INTEGRATORS}
          value={design.integrator}
          onChange={(integrator) => {
            set({ integrator });
          }}
        />

        {galaxy && (
          <p className={styles.note}>
            The softening reaches the sampler as well as the solver. A disc is placed on
            the circular orbits the run’s own force law supports, so changing it changes
            the galaxy and not only the forces (ADR-0038).
          </p>
        )}
      </section>
    </div>
  );
}
