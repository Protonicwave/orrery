/**
 * A design: the settings a configuration file states, held in one record.
 *
 * The editor edits the parameters the format actually has rather than a set
 * invented for the interface, so that what is on screen and what is in the file
 * are the same numbers. That is what makes the download worth having: a design
 * leaves here as an `.orrery` file the native binary runs unmodified.
 *
 * One record with the union of every generator's parameters rather than a
 * variant, which is the arrangement `sim/configuration.hpp` gives for its own
 * reasons and which happens to be what the presets need: switching from a
 * Plummer sphere to a disc galaxy keeps the count, the mass and the run
 * settings, so the four presets are one path rather than four starting points.
 */

export type DesignKind = 'kepler' | 'plummer' | 'disc-galaxy' | 'galaxy-collision';

export type SolverKind = 'direct' | 'barnes-hut';

export type IntegratorKind = 'velocity-verlet' | 'yoshida4' | 'rk4';

/**
 * Everything the editor can set, named as the configuration format names it.
 *
 * The names are the file's, in the file's units: angles in radians, lengths and
 * masses in the units where G is one (ADR-0007). The interface shows degrees
 * beside an angle because that is how a drawing is dimensioned, and the
 * conversion happens where it is shown rather than here, so that nothing has to
 * remember which of the two a stored number is.
 */
export interface Design {
  readonly kind: DesignKind;

  readonly timestep: number;
  readonly steps: number;
  readonly seed: number;

  /** The particles a sampled configuration draws. Kepler states none. */
  readonly count: number;
  readonly totalMass: number;

  /** The Plummer scale radius, or zero for the standard one. */
  readonly scaleRadius: number;
  readonly massFractionCutoff: number;

  readonly primaryMass: number;
  readonly secondaryMass: number;
  readonly semiMajorAxis: number;
  readonly eccentricity: number;

  readonly bulgeFraction: number;
  readonly scaleLength: number;
  readonly scaleHeight: number;
  readonly bulgeRadius: number;
  readonly inclination: number;
  readonly positionAngle: number;

  readonly massRatio: number;
  readonly secondaryInclination: number;
  readonly secondaryPositionAngle: number;
  readonly separation: number;
  readonly impactParameter: number;
  readonly approachSpeed: number;

  readonly solver: SolverKind;
  readonly softening: number;
  readonly openingAngle: number;
  readonly integrator: IntegratorKind;
}

/** Whether the design draws its particles from a distribution. */
export function isSampled(kind: DesignKind): boolean {
  return kind !== 'kepler';
}

/** Whether the design has a disc, and so a shape and a spin axis. */
export function isGalaxy(kind: DesignKind): boolean {
  return kind === 'disc-galaxy' || kind === 'galaxy-collision';
}

/**
 * The four presets, as one path.
 *
 * Each is the next one's starting point: the two-body problem is the case that
 * can be solved exactly, the Plummer sphere is the equilibrium many-body case,
 * the disc is that sphere given rotation and a shape, and the collision is two
 * discs. Everything a step of the path does not use is still in the record, at
 * the value the step before it left, so moving along the path never asks for a
 * number twice.
 *
 * The values come from the configurations in `examples/`, which are the ones
 * the reports and the validation were run with. The disc is the one preset with
 * no example of its own, and it is the collision's primary galaxy on its own:
 * the same three lengths, the same bulge share and the same softening.
 */
const BASE: Design = {
  kind: 'galaxy-collision',
  timestep: 0.005,
  steps: 40000,
  seed: 20260812,
  count: 60000,
  totalMass: 1.5,
  scaleRadius: 0,
  massFractionCutoff: 0.99,
  primaryMass: 1,
  secondaryMass: 1,
  semiMajorAxis: 1,
  eccentricity: 0.5,
  bulgeFraction: 0.2,
  scaleLength: 1,
  scaleHeight: 0.1,
  bulgeRadius: 0.2,
  inclination: 0.35,
  positionAngle: 0,
  massRatio: 0.5,
  secondaryInclination: 1.2,
  secondaryPositionAngle: 0,
  separation: 20,
  impactParameter: 2,
  approachSpeed: 0.8,
  solver: 'barnes-hut',
  softening: 0.12,
  openingAngle: 0.6,
  integrator: 'velocity-verlet',
};

export interface Preset {
  readonly id: DesignKind;
  readonly title: string;
  /** One sentence on what the configuration is for. */
  readonly note: string;
  readonly design: Design;
}

export const PRESETS: readonly Preset[] = [
  {
    id: 'kepler',
    title: 'Kepler two-body',
    note: 'The only gravitational system anyone can solve exactly, and so the project’s primary validation instrument. Two point masses released at periapsis, with no softening.',
    design: {
      ...BASE,
      kind: 'kepler',
      timestep: 0.02221441469,
      steps: 40000,
      seed: 1,
      // Two bodies by definition, and the reader rejects a count stated beside
      // this kind rather than ignoring one.
      count: 0,
      solver: 'direct',
      softening: 0,
    },
  },
  {
    id: 'plummer',
    title: 'Plummer sphere',
    note: 'A cluster in equilibrium: its density and its velocities solve the same equation, so a sample of it stays as it was drawn. This is the configuration the conservation tests hold a run to.',
    design: {
      ...BASE,
      kind: 'plummer',
      timestep: 0.001,
      steps: 1000,
      seed: 20260811,
      count: 4096,
      totalMass: 1,
      massFractionCutoff: 0.999,
      solver: 'barnes-hut',
      softening: 0.05,
      openingAngle: 0.5,
    },
  },
  {
    id: 'disc-galaxy',
    title: 'Single disc',
    note: 'A Plummer bulge inside an exponential disc, every disc particle on the circular orbit its enclosed mass supports. It is cold, so it develops spiral structure and a bar within a few rotations (ADR-0038).',
    design: {
      ...BASE,
      kind: 'disc-galaxy',
      steps: 20000,
      count: 30000,
      totalMass: 1,
    },
  },
  {
    id: 'galaxy-collision',
    title: 'Galaxy collision',
    note: 'Two discs on a bound encounter, placed on the two-body orbit their total masses would follow if each were a point. This is the project’s demonstration configuration.',
    design: BASE,
  },
];

/** The preset a kind starts from. */
export function preset(kind: DesignKind): Design {
  const found = PRESETS.find((entry) => entry.id === kind);
  if (found === undefined) throw new Error(`${kind} is not a preset`);
  return found.design;
}

/**
 * The design with one kind exchanged for another, keeping everything else.
 *
 * Only the settings the new kind needs and the old one had no opinion about are
 * taken from its preset. A count carried from a Plummer sphere into a collision
 * is a count somebody chose; a timestep carried there is a timestep chosen for a
 * cluster, and the two scenarios move at different speeds, so the run settings
 * follow the kind.
 */
export function withKind(design: Design, kind: DesignKind): Design {
  const next = preset(kind);
  return {
    ...design,
    kind,
    timestep: next.timestep,
    steps: next.steps,
    softening: next.softening,
    solver: next.solver,
    openingAngle: next.openingAngle,
    massFractionCutoff: next.massFractionCutoff,
    // A two-body configuration is two bodies whatever was set before it, and
    // the reader rejects a count beside it rather than ignoring one. Coming the
    // other way there is no count to keep, so the preset's is taken.
    count: kind === 'kepler' ? 0 : design.count >= 4 ? design.count : next.count,
  };
}

/**
 * What the C++ reader would refuse, in its own words.
 *
 * The client checks what the C++ checks so that a file this editor offers for
 * download is a file the binary accepts. The wording is taken from
 * `sim/configuration.cpp` rather than rewritten, because two messages for one
 * condition is two things to keep in step.
 *
 * The controls are bounded so that most of these cannot be reached by operating
 * the interface. They are checked anyway: a design can also arrive from the
 * address bar, and a rule that is only enforced by a slider's range is not
 * enforced.
 */
export function problemsWith(design: Design): readonly string[] {
  const problems: string[] = [];
  const add = (setting: string, problem: string): void => {
    problems.push(`${setting} ${problem}`);
  };

  if (!(design.timestep > 0)) add('run.timestep', 'must be a positive number');
  if (!(design.steps >= 1)) add('run.steps', 'must be at least one');

  if (isSampled(design.kind)) {
    if (!(design.count >= 2)) {
      add(
        'initial_conditions.count',
        'must be at least two for a sampled configuration',
      );
    }
    if (!(design.totalMass > 0)) {
      add('initial_conditions.total_mass', 'must be a positive number');
    }
  }

  if (design.kind === 'plummer' || isGalaxy(design.kind)) {
    if (!(design.massFractionCutoff > 0) || !(design.massFractionCutoff < 1)) {
      add('initial_conditions.mass_fraction_cutoff', 'must lie strictly in (0, 1)');
    }
  }

  if (isGalaxy(design.kind)) {
    if (!(design.bulgeFraction >= 0) || !(design.bulgeFraction < 1)) {
      add('initial_conditions.bulge_fraction', 'must lie in [0, 1)');
    }
    if (!(design.scaleLength > 0)) {
      add('initial_conditions.scale_length', 'must be a positive number');
    }
    if (!(design.scaleHeight > 0)) {
      add('initial_conditions.scale_height', 'must be a positive number');
    }
    if (!(design.bulgeRadius > 0)) {
      add('initial_conditions.bulge_radius', 'must be a positive number');
    }
  }

  if (design.kind === 'galaxy-collision') {
    if (!(design.massRatio > 0) || !(design.massRatio <= 1)) {
      add('initial_conditions.mass_ratio', 'must lie in (0, 1]');
    }
    if (design.separation === 0 && design.impactParameter === 0) {
      add(
        'initial_conditions.separation',
        'and initial_conditions.impact_parameter cannot both be zero, since the two galaxies would start on top of one another',
      );
    }
    if (!(design.approachSpeed >= 0)) {
      add('initial_conditions.approach_speed', 'must not be negative');
    }
    if (!(design.count >= 4)) {
      add(
        'initial_conditions.count',
        'must be at least four for a collision, which is two galaxies',
      );
    }
  }

  if (design.kind === 'plummer' && design.scaleRadius < 0) {
    add('initial_conditions.scale_radius', 'must not be negative');
  }

  if (design.kind === 'kepler') {
    if (!(design.primaryMass > 0)) {
      add('initial_conditions.primary_mass', 'must be a positive number');
    }
    if (!(design.secondaryMass > 0)) {
      add('initial_conditions.secondary_mass', 'must be a positive number');
    }
    if (!(design.semiMajorAxis > 0)) {
      add('initial_conditions.semi_major_axis', 'must be a positive number');
    }
    if (!(design.eccentricity >= 0) || !(design.eccentricity < 1)) {
      add('initial_conditions.eccentricity', 'must lie in [0, 1) for a bound orbit');
    }
    if (design.count !== 0) {
      add(
        'initial_conditions.count',
        'is not used by the kepler configuration, which is always two bodies',
      );
    }
  }

  if (!(design.softening >= 0)) add('solver.softening', 'must not be negative');
  if (!(design.openingAngle >= 0)) add('solver.opening_angle', 'must not be negative');

  return problems;
}

/**
 * A number as the file states it.
 *
 * The shortest text that reads back as the same double, which is what
 * JavaScript's own conversion gives. The C++ reader parses a real through a
 * stream in the classic locale, so it accepts the exponent form this can
 * produce for a very small value, and rejects anything with a character left
 * over, which this cannot produce at all.
 */
function real(value: number): string {
  if (!Number.isFinite(value)) {
    throw new Error(`${value} cannot be written to a configuration file`);
  }
  return String(value);
}

/** A count as the file states it, which is a whole number and never 4e3. */
function integer(value: number): string {
  return String(Math.round(value));
}

/** The settings a kind uses, in the order `write_configuration` writes them. */
function initialConditions(design: Design): readonly (readonly [string, string])[] {
  const settings: [string, string][] = [['kind', design.kind]];

  if (isSampled(design.kind)) {
    settings.push(
      ['count', integer(design.count)],
      ['total_mass', real(design.totalMass)],
    );
  }
  if (design.kind === 'plummer' && design.scaleRadius > 0) {
    settings.push(['scale_radius', real(design.scaleRadius)]);
  }
  if (design.kind === 'plummer' || isGalaxy(design.kind)) {
    settings.push(['mass_fraction_cutoff', real(design.massFractionCutoff)]);
  }
  if (design.kind === 'kepler') {
    settings.push(
      ['primary_mass', real(design.primaryMass)],
      ['secondary_mass', real(design.secondaryMass)],
      ['semi_major_axis', real(design.semiMajorAxis)],
      ['eccentricity', real(design.eccentricity)],
    );
  }
  if (isGalaxy(design.kind)) {
    settings.push(
      ['bulge_fraction', real(design.bulgeFraction)],
      ['scale_length', real(design.scaleLength)],
      ['scale_height', real(design.scaleHeight)],
      ['bulge_radius', real(design.bulgeRadius)],
      ['inclination', real(design.inclination)],
    );
    if (design.positionAngle !== 0) {
      settings.push(['position_angle', real(design.positionAngle)]);
    }
  }
  if (design.kind === 'galaxy-collision') {
    settings.push(
      ['mass_ratio', real(design.massRatio)],
      ['secondary_inclination', real(design.secondaryInclination)],
    );
    if (design.secondaryPositionAngle !== 0) {
      settings.push(['secondary_position_angle', real(design.secondaryPositionAngle)]);
    }
    settings.push(
      ['separation', real(design.separation)],
      ['impact_parameter', real(design.impactParameter)],
      ['approach_speed', real(design.approachSpeed)],
    );
  }

  return settings;
}

/** How the output section names the files a run of this design would write. */
function outputStem(kind: DesignKind): string {
  return kind === 'galaxy-collision'
    ? 'collision'
    : kind === 'disc-galaxy'
      ? 'disc'
      : kind;
}

/**
 * How often a run of `steps` steps should record a frame and a diagnostic.
 *
 * A few hundred frames and a few hundred rows, whatever the length of the run,
 * which is what the examples choose and what makes a trajectory a size worth
 * keeping. Written out rather than left to the default so that the file says
 * what it will produce.
 */
function strides(steps: number): { trajectory: number; diagnostics: number } {
  return {
    trajectory: Math.max(1, Math.round(steps / 1000)),
    diagnostics: Math.max(1, Math.round(steps / 100)),
  };
}

const SECTION_BREAK = '';

/**
 * The design as the text of an `.orrery` file.
 *
 * The header is the comment block the examples carry, which is the part of one
 * of those files worth having: it says what the configuration is and what to
 * run it with. Every figure in it is computed from the design by the functions
 * in `elements.ts`, so the prose cannot drift from the settings under it.
 *
 * `describe` provides that prose. It is a parameter rather than an import
 * because the elements are derived from the settings and the settings should
 * not depend on their own description. Left out, the file carries no header,
 * which is what the preview wants: the module is handed the settings and has
 * nobody to read a comment.
 */
export function writeDesign(
  design: Design,
  describe?: (design: Design) => string,
): string {
  const lines: string[] = [];
  if (describe !== undefined) {
    for (const line of describe(design).split('\n')) {
      lines.push(line === '' ? '#' : `# ${line}`);
    }
    lines.push(SECTION_BREAK);
  }

  lines.push('[run]');
  lines.push(`timestep = ${real(design.timestep)}`);
  lines.push(`steps = ${integer(design.steps)}`);
  lines.push(`seed = ${integer(design.seed)}`);
  lines.push(SECTION_BREAK);

  lines.push('[initial_conditions]');
  for (const [key, value] of initialConditions(design)) lines.push(`${key} = ${value}`);
  lines.push(SECTION_BREAK);

  lines.push('[solver]');
  lines.push(`kind = ${design.solver}`);
  lines.push(`softening = ${real(design.softening)}`);
  if (design.solver === 'barnes-hut') {
    lines.push(`opening_angle = ${real(design.openingAngle)}`);
  }
  lines.push(SECTION_BREAK);

  lines.push('[integrator]');
  lines.push(`kind = ${design.integrator}`);
  lines.push(SECTION_BREAK);

  const stem = outputStem(design.kind);
  const stride = strides(design.steps);
  lines.push('[output]');
  lines.push(`trajectory_path = ${stem}.otj`);
  lines.push(`trajectory_stride = ${integer(stride.trajectory)}`);
  lines.push(`diagnostics_path = ${stem}.csv`);
  lines.push(`diagnostics_stride = ${integer(stride.diagnostics)}`);
  lines.push('');

  return lines.join('\n');
}

/** What a downloaded design is called. */
export function designFilename(design: Design): string {
  return `${outputStem(design.kind)}-${integer(design.seed)}.orrery`;
}

/**
 * The command that runs a downloaded design, for the header and for the
 * interface to show beside the button.
 */
export function runCommand(design: Design): string {
  return `orrery run ${designFilename(design)}`;
}
