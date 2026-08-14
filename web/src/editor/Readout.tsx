/**
 * biome-ignore-all lint/a11y/noNoninteractiveTabindex: the file scrolls, and a
 * region that scrolls has to be focusable or a keyboard cannot read it, which
 * is what the audit in `e2e/editor.spec.ts` requires.
 */

import { Numeric } from '../components/Numeric';
import type { BrowserRun } from '../solver/configure';
import type { Achieved } from '../solver/run';
import type { TrajectoryStatus } from '../trajectory/client';
import type { Design } from './design';
import { designFilename, runCommand } from './design';
import {
  circularSpeed,
  components,
  cutoffRadius,
  encounterOf,
  keplerElements,
  particleMass,
  primaryGalaxy,
  resolvedScaleRadius,
  secondaryGalaxy,
  singleGalaxy,
  totalMass,
} from './elements';
import styles from './Readout.module.css';

/** One line of the readout: what it is, and the value. */
function Line({ name, children }: { name: string; children: React.ReactNode }) {
  return (
    <div className={styles.line}>
      <dt>{name}</dt>
      <dd>{children}</dd>
    </div>
  );
}

function CollisionElements({ design }: { design: Design }) {
  const encounter = encounterOf(design);
  const primary = components(primaryGalaxy(design));
  const secondary = components(secondaryGalaxy(design));

  return (
    <>
      <Line name="Encounter">
        <span className={encounter.bound ? styles.bound : styles.unbound}>
          {encounter.bound ? 'bound · merges' : 'unbound · passes once'}
        </span>
      </Line>
      <Line name="Orbit energy">
        <Numeric value={encounter.energy} notation="scientific" digits={2} />
      </Line>
      <Line name="Eccentricity">
        <Numeric value={encounter.eccentricity} digits={3} />
      </Line>
      <Line name="Periapsis">
        <Numeric value={encounter.periapsis} digits={3} />
      </Line>
      <Line name="To encounter">
        {Number.isFinite(encounter.timeToEncounter) ? (
          <Numeric value={encounter.timeToEncounter} digits={2} />
        ) : (
          'no closest approach'
        )}
      </Line>
      <Line name="Escape speed">
        <Numeric value={encounter.escapeSpeed} digits={3} />
      </Line>
      <Line name="Relative speed">
        <Numeric value={encounter.speed} digits={3} />
      </Line>
      <Line name="Primary">
        <Numeric value={primary.discCount + primary.bulgeCount} /> at{' '}
        <Numeric value={totalMass(primaryGalaxy(design))} digits={3} />
      </Line>
      <Line name="Secondary">
        <Numeric value={secondary.discCount + secondary.bulgeCount} /> at{' '}
        <Numeric value={totalMass(secondaryGalaxy(design))} digits={3} />
      </Line>
      <Line name="Particle mass">
        <Numeric value={particleMass(design)} notation="scientific" digits={2} />
      </Line>
    </>
  );
}

function DiscElements({ design }: { design: Design }) {
  const galaxy = singleGalaxy(design);
  const parts = components(galaxy);

  return (
    <>
      <Line name="Disc">
        <Numeric value={parts.discCount} /> at{' '}
        <Numeric value={parts.discMass} digits={3} />
      </Line>
      <Line name="Bulge">
        <Numeric value={parts.bulgeCount} /> at{' '}
        <Numeric value={parts.bulgeMass} digits={3} />
      </Line>
      <Line name="Particle mass">
        <Numeric value={parts.particleMass} notation="scientific" digits={2} />
      </Line>
      <Line name="Disc edge">
        <Numeric value={cutoffRadius(galaxy)} digits={2} />
      </Line>
      <Line name="Speed at R_d">
        <Numeric value={circularSpeed(galaxy, galaxy.scaleLength)} digits={3} />
      </Line>
      <Line name="Speed at edge">
        <Numeric value={circularSpeed(galaxy, cutoffRadius(galaxy))} digits={3} />
      </Line>
    </>
  );
}

function PlummerElements({ design }: { design: Design }) {
  const radius = resolvedScaleRadius(design);
  const share = design.massFractionCutoff ** (2 / 3);

  return (
    <>
      <Line name="Scale radius">
        <Numeric value={radius} digits={3} />
      </Line>
      <Line name="Sampled to">
        <Numeric value={radius * Math.sqrt(share / (1 - share))} digits={2} />
      </Line>
      <Line name="Particle mass">
        <Numeric value={particleMass(design)} notation="scientific" digits={2} />
      </Line>
    </>
  );
}

function KeplerElementsPanel({ design }: { design: Design }) {
  const elements = keplerElements(design);

  return (
    <>
      <Line name="Period">
        <Numeric value={elements.period} digits={3} />
      </Line>
      <Line name="Periapsis">
        <Numeric value={elements.periapsis} digits={3} />
      </Line>
      <Line name="Speed there">
        <Numeric value={elements.speed} digits={3} />
      </Line>
      <Line name="Energy">
        <Numeric value={elements.energy} digits={4} />
      </Line>
      <Line name="Angular mom.">
        <Numeric value={elements.angularMomentum} digits={4} />
      </Line>
      <Line name="Steps a turn">
        <Numeric value={elements.period / design.timestep} digits={1} />
      </Line>
    </>
  );
}

export interface ReadoutProps {
  design: Design;
  /** The text the download would carry, so the two cannot differ. */
  text: string;
  problems: readonly string[];
  /** What the preview is doing, or nulls when it has not been started. */
  preview: {
    readonly running: boolean;
    readonly status: TrajectoryStatus | null;
    readonly message: string;
    readonly plan: BrowserRun | null;
    readonly achieved: Achieved | null;
    readonly onStart: () => void;
    readonly onStop: () => void;
  };
}

/**
 * The rail: what the design comes to, and the file it makes.
 *
 * Every figure here is derived from the settings by `elements.ts`, which is a
 * transcription of the C++ that will sample the design. None of it is measured,
 * because nothing has been run: what a preview measures appears separately and
 * says so, in the same way the plate states the conditions a picture was taken
 * under.
 */
export function Readout({ design, text, problems, preview }: ReadoutProps) {
  const download = (): void => {
    const blob = new Blob([text], { type: 'text/plain;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = designFilename(design);
    link.click();
    URL.revokeObjectURL(url);
  };

  const achieved = preview.achieved;

  return (
    <aside className={styles.rail} aria-label="What the design comes to">
      <section className={styles.panel}>
        <h2 className="label">Elements</h2>
        <dl className={styles.readout}>
          {design.kind === 'galaxy-collision' && <CollisionElements design={design} />}
          {design.kind === 'disc-galaxy' && <DiscElements design={design} />}
          {design.kind === 'plummer' && <PlummerElements design={design} />}
          {design.kind === 'kepler' && <KeplerElementsPanel design={design} />}
          <Line name="Model time">
            <Numeric value={design.steps * design.timestep} digits={2} />
          </Line>
        </dl>
        <p className={styles.note}>
          Worked out from the settings by the same formulae the sampler uses, before
          anything is run. For a pair of galaxies they are the orbit the placement puts
          them on, each treated as a point mass, which is exact only while the two are
          far apart.
        </p>
      </section>

      <section className={styles.panel}>
        <h2 className="label">Preview</h2>
        <button
          type="button"
          className={styles.action}
          onClick={preview.running ? preview.onStop : preview.onStart}
        >
          <span>
            {preview.running ? 'Stop the preview' : 'Sample and step it here'}
          </span>
          <span className={styles.price}>
            {preview.plan === null
              ? 'WebAssembly · the same solver'
              : `${preview.plan.count} bodies · ${preview.plan.steps} steps`}
          </span>
        </button>

        {achieved !== null && achieved.count > 0 && (
          <dl className={styles.readout}>
            <Line name="Sampled">
              <Numeric value={achieved.count} />
            </Line>
            <Line name="State">{preview.status ?? 'not started'}</Line>
            <Line name="Solver">
              {achieved.solver}, {achieved.kernel}, 1 thread
            </Line>
            <Line name="Step">
              <Numeric value={achieved.stepMilliseconds} digits={1} unit="ms" />
            </Line>
            <Line name="dE/E">
              {achieved.energyDrift === null ? (
                'pending'
              ) : (
                <Numeric
                  value={achieved.energyDrift}
                  notation="scientific"
                  digits={2}
                />
              )}
            </Line>
            <Line name="Virial">
              {achieved.virialRatio === null ? (
                'pending'
              ) : (
                <Numeric value={achieved.virialRatio} digits={3} />
              )}
            </Line>
          </dl>
        )}

        <p className={styles.note}>
          {preview.message !== ''
            ? preview.message
            : 'The same C++ as the native binary, compiled to WebAssembly and stepped in a Worker, on one thread and with the scalar kernel. It is cut to a few thousand particles and the first part of the run, so it shows the physics of the design rather than the picture the whole run would make.'}
        </p>
      </section>

      <section className={styles.panel}>
        <h2 className="label">The file</h2>
        {problems.length > 0 && (
          <ul className={styles.problems}>
            {problems.map((problem) => (
              <li key={problem}>{problem}</li>
            ))}
          </ul>
        )}
        {/* A region rather than a bare block, because it scrolls: a region a
            pointer can scroll and a keyboard cannot is a region a keyboard
            cannot read, and a scrollable thing has to be focusable to be
            scrolled by one. */}
        <section
          className={styles.file}
          aria-label="The configuration file"
          tabIndex={0}
        >
          <pre>{text}</pre>
        </section>
        <button
          type="button"
          className={styles.action}
          disabled={problems.length > 0}
          onClick={download}
        >
          <span>Download {designFilename(design)}</span>
          <span className={styles.price}>{runCommand(design)}</span>
        </button>
        <p className={styles.note}>
          The file the native binary reads, with nothing left out and nothing added.
          What is above is what is downloaded.
        </p>
      </section>
    </aside>
  );
}
