/**
 * The comment block a downloaded design carries.
 *
 * Every example configuration in the repository opens with a paragraph saying
 * what it is and what to look at, and that header is the most useful part of
 * those files. A design that left here as bare settings would be a worse
 * document than the ones it sits beside, so it is written the same way.
 *
 * Every figure in the prose is computed from the design by `elements.ts`, so
 * the description cannot drift from the settings under it. Nothing is stated
 * that is not derived: what the run will do is what the run will do, and this
 * file says what the configuration is.
 */

import { scientific } from '../format/number';
import type { Design } from './design';
import { isGalaxy, runCommand } from './design';
import {
  circularSpeed,
  components,
  cutoffRadius,
  encounterOf,
  keplerElements,
  particleMass,
  primaryGalaxy,
  resolvedScaleRadius,
  singleGalaxy,
} from './elements';

/**
 * A number as a comment states it: plain digits, no grouping and no thin
 * spaces, because this is read by whoever opens the file in an editor and the
 * file is otherwise ASCII. Anything below a hundredth is set in the exponent
 * form, since a quantity like an orbit energy is small and stating it as 0.009
 * would throw away most of what is known about it.
 */
function plain(value: number, digits = 3): string {
  if (value === 0) return '0';
  if (Math.abs(value) >= 0.01 && Math.abs(value) < 1e6) {
    const fixed = value.toFixed(digits);
    // Trailing zeros go, and only after a decimal point: a run of two hundred
    // steps written as two would be a quietly wrong file.
    return fixed.includes('.') ? fixed.replace(/0+$/, '').replace(/\.$/, '') : fixed;
  }
  const parts = scientific(value, 2);
  return `${parts.mantissa}e${parts.exponent}`.replace(/[−]/g, '-');
}

/** A block of aligned name and value pairs, indented as a code block is. */
function table(rows: readonly (readonly [string, string])[]): string {
  const width = Math.max(...rows.map(([name]) => name.length));
  return rows.map(([name, value]) => `    ${name.padEnd(width)}  ${value}`).join('\n');
}

function keplerDescription(design: Design): string {
  const elements = keplerElements(design);
  return [
    'The two-body problem: two point masses on a bound orbit, released at',
    'periapsis. It is the only gravitational system with an exact solution, so it',
    'is what an integrator is measured against rather than compared with.',
    '',
    'The elements of this orbit are',
    '',
    table([
      ['period', plain(elements.period)],
      ['periapsis', plain(elements.periapsis)],
      ['energy', plain(elements.energy)],
      ['angular momentum', plain(elements.angularMomentum)],
      ['speed at periapsis', plain(elements.speed)],
    ]),
    '',
    `and the timestep below is ${plain(elements.period / design.timestep, 1)} steps to the revolution, over`,
    `${plain(design.steps / (elements.period / design.timestep), 1)} revolutions. What to watch in the diagnostics is the shape of the`,
    'relative energy error rather than its size: a symplectic integrator holds it',
    'inside an envelope and stays there (ADR-0011).',
  ].join('\n');
}

function plummerDescription(design: Design): string {
  const radius = resolvedScaleRadius(design);
  return [
    'A Plummer sphere: a cluster in equilibrium, which is what makes it the',
    'reference configuration. Its density and its distribution of velocities solve',
    'the collisionless Boltzmann equation together, so a sample of it stays as it',
    'was drawn and a conservation test can hold a run to a number.',
    '',
    table([
      ['particles', plain(design.count, 0)],
      ['total mass', plain(design.totalMass)],
      ['particle mass', plain(particleMass(design))],
      ['scale radius', plain(radius)],
      ['mass sampled', `${plain(design.massFractionCutoff * 100, 1)} per cent`],
    ]),
    '',
    ...(design.scaleRadius > 0
      ? [
          'The scale radius is stated rather than left to the default, which is the',
          '3 pi / 16 that puts a unit-mass sphere into standard N-body units.',
        ]
      : [
          'The scale radius is the default, 3 pi / 16, which puts a unit-mass sphere into',
          'standard N-body units and lets a figure from this run be put beside a published',
          'one without a scaling factor between them.',
        ]),
  ].join('\n');
}

function discDescription(design: Design): string {
  const galaxy = singleGalaxy(design);
  const parts = components(galaxy);
  const edge = cutoffRadius(galaxy);
  return [
    'A disc galaxy: a Plummer bulge inside an exponential disc, every disc particle',
    'placed on the circular orbit its enclosed mass supports.',
    '',
    table([
      [
        'particles',
        `${plain(parts.discCount, 0)} disc, ${plain(parts.bulgeCount, 0)} bulge`,
      ],
      ['particle mass', plain(parts.particleMass)],
      ['scale length', plain(galaxy.scaleLength)],
      ['scale height', plain(galaxy.scaleHeight)],
      ['bulge radius', plain(galaxy.bulgeRadius)],
      ['disc edge', plain(edge)],
      ['circular speed at R_d', plain(circularSpeed(galaxy, galaxy.scaleLength))],
    ]),
    '',
    'This is a scenario rather than an equilibrium, and ADR-0038 says in what three',
    'ways. The disc is cold, so it is unstable to its own self-gravity and develops',
    'spiral structure and a bar within a few rotations, which is the behaviour it is',
    'here to show. The softening confines the fragmentation that comes with that to',
    'scales below the scale height, which is the smallest structure the model has.',
  ].join('\n');
}

function collisionDescription(design: Design): string {
  const encounter = encounterOf(design);
  const primary = primaryGalaxy(design);
  const parts = components(primary);
  const outcome = encounter.bound
    ? 'bound, so the pair falls together, passes and returns'
    : 'unbound, so the pair passes once and separates for ever';

  return [
    'Two disc galaxies on an encounter, placed on the two-body orbit their total',
    'masses would follow if each were a point.',
    '',
    `The relative speed is ${plain(design.approachSpeed, 2)} of the escape speed at the initial separation,`,
    `which makes the encounter ${outcome}.`,
    '',
    table([
      [
        'particles',
        `${plain(design.count, 0)}, mass ratio ${plain(design.massRatio, 2)}`,
      ],
      ['particle mass', plain(parts.particleMass)],
      ['distance apart', plain(encounter.separation)],
      ['impact parameter', plain(design.impactParameter)],
      ['relative speed', plain(encounter.speed)],
      ['orbit energy', plain(encounter.energy)],
      ['eccentricity', plain(encounter.eccentricity)],
      ['periapsis', plain(encounter.periapsis)],
      ['time to encounter', plain(encounter.timeToEncounter, 1)],
    ]),
    '',
    'Those elements are the placement, not a prediction of the run. Each galaxy is',
    'treated as a point mass, which is exact only while the two are far apart, so',
    'what happens after the first passage is what the integration says happens.',
    '',
    'The two discs are inclined differently on purpose. An encounter between two',
    'discs in the same plane is symmetric and the least interesting to look at; the',
    'tidal tails are drawn out of whichever disc is closest to coplanar with the',
    'orbit, so a pair inclined by different amounts produces one long tail and one',
    'stubby one, which is what real interacting pairs look like.',
  ].join('\n');
}

/**
 * The whole comment block, with the command that runs the file at the foot of
 * it. The README's standard, kept here: a figure is stated beside the command
 * that reproduces it.
 */
export function describeDesign(design: Design): string {
  const body =
    design.kind === 'kepler'
      ? keplerDescription(design)
      : design.kind === 'plummer'
        ? plummerDescription(design)
        : design.kind === 'disc-galaxy'
          ? discDescription(design)
          : collisionDescription(design);

  const softening = isGalaxy(design.kind)
    ? [
        `The softening is ${plain(design.softening, 3)}, and it reaches the sampler as well as the solver,`,
        'because a disc has to be built at the speeds the forces it will feel can',
        'support (ADR-0038).',
      ]
    : [];

  return [
    body,
    '',
    `The run below is ${plain(design.steps, 0)} steps of ${plain(design.timestep, 6)}, which is ${plain(design.steps * design.timestep, 2)} of model time.`,
    ...softening,
    '',
    'Run it with',
    '',
    `    ${runCommand(design)}`,
  ].join('\n');
}
