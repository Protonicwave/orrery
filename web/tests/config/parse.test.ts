import { describe, expect, it } from 'vitest';
import {
  ConfigurationError,
  identifier,
  numeric,
  override,
  parseConfiguration,
  setting,
  writeConfiguration,
} from '../../src/config/parse';

describe('the configuration reader', () => {
  it('reads sections and settings', () => {
    const configuration = parseConfiguration(`
[run]
timestep = 0.005
steps    = 40000
`);
    expect(setting(configuration, 'run', 'timestep')).toBe('0.005');
    expect(numeric(configuration, 'run', 'steps')).toBe(40000);
  });

  it('ignores blank lines and comments, and keeps a hash inside a value', () => {
    const configuration = parseConfiguration(`
# a comment

[output]
trajectory_path = runs/one#two.otj
`);
    expect(setting(configuration, 'output', 'trajectory_path')).toBe(
      'runs/one#two.otj',
    );
  });

  it('keeps whitespace inside a value and removes it around one', () => {
    const configuration = parseConfiguration('[a]\nk =  two words  ');
    expect(setting(configuration, 'a', 'k')).toBe('two words');
  });

  it('accepts a key that names its own section without changing the section', () => {
    const configuration = parseConfiguration(`
solver.softening = 0.05
[run]
steps = 10
`);
    expect(setting(configuration, 'solver', 'softening')).toBe('0.05');
    expect(setting(configuration, 'run', 'steps')).toBe('10');
  });

  it('reports a setting outside any section by line', () => {
    expect(() => parseConfiguration('\nsteps = 10')).toThrow(ConfigurationError);
    try {
      parseConfiguration('\nsteps = 10');
    } catch (error) {
      expect((error as ConfigurationError).line).toBe(2);
    }
  });

  it('rejects the same setting given twice, in either spelling', () => {
    expect(() => parseConfiguration('[run]\nsteps = 1\nrun.steps = 2')).toThrow(
      /set twice/,
    );
  });

  it('rejects a setting with no value and a line that is not one', () => {
    expect(() => parseConfiguration('[run]\nsteps =')).toThrow(/no value/);
    expect(() => parseConfiguration('[run]\nsteps')).toThrow(/expected a setting/);
  });

  it('rejects a number with anything after it, as the C++ reader does', () => {
    const configuration = parseConfiguration('[run]\ntimestep = 0.5 and a bit');
    expect(() => numeric(configuration, 'run', 'timestep')).toThrow(/not a number/);
  });

  it('accepts the exponent form', () => {
    const configuration = parseConfiguration('[solver]\nsoftening = 1.5e-3');
    expect(numeric(configuration, 'solver', 'softening')).toBe(0.0015);
  });

  it('leaves an unstated setting undefined rather than guessing a default', () => {
    const configuration = parseConfiguration('[run]\nsteps = 1');
    expect(setting(configuration, 'run', 'seed')).toBeUndefined();
    expect(numeric(configuration, 'run', 'seed')).toBeUndefined();
  });
});

describe('the configuration writer', () => {
  it('writes what the reader reads back', () => {
    const text = `
# A comment, and a blank line.

[run]
timestep = 0.001
steps    = 1000

[solver]
kind = direct
`;
    const configuration = parseConfiguration(text);
    expect(parseConfiguration(writeConfiguration(configuration))).toEqual(
      configuration,
    );
  });

  it('carries an override through', () => {
    const configuration = override(parseConfiguration('[run]\nsteps = 1000\n'), [
      { setting: 'run.steps', value: '200' },
      { setting: 'initial_conditions.count', value: '64' },
    ]);

    const written = parseConfiguration(writeConfiguration(configuration));
    expect(setting(written, 'run', 'steps')).toBe('200');
    expect(setting(written, 'initial_conditions', 'count')).toBe('64');
  });

  it('writes a section that has no settings in it', () => {
    // The reader records an empty section, and a writer that dropped it would
    // turn a document into a different one.
    const configuration = parseConfiguration('[integrator]\n');
    expect(writeConfiguration(configuration)).toContain('[integrator]');
    expect(parseConfiguration(writeConfiguration(configuration))).toEqual(
      configuration,
    );
  });
});

describe('the run identifier', () => {
  it('is a function of the file, so the same file earns the same name', () => {
    expect(identifier('[run]\nsteps = 1', 20260812)).toBe(
      identifier('[run]\nsteps = 1', 20260812),
    );
  });

  it('changes when the file changes', () => {
    expect(identifier('[run]\nsteps = 1', 7)).not.toBe(
      identifier('[run]\nsteps = 2', 7),
    );
  });

  it('carries the seed and four hexadecimal digits', () => {
    expect(identifier('anything', 20260812)).toMatch(/^20260812-[0-9A-F]{4}$/);
  });
});

describe('applying overrides', () => {
  const file = parseConfiguration(
    `[run]
steps = 100
seed = 1
[solver]
softening = 0.12`,
  );

  it('replaces a setting the file states, as --set does', () => {
    const run = override(file, [{ setting: 'run.steps', value: '40000' }]);
    expect(numeric(run, 'run', 'steps')).toBe(40000);
    expect(numeric(run, 'run', 'seed')).toBe(1);
    expect(numeric(run, 'solver', 'softening')).toBe(0.12);
  });

  it('adds a setting the file leaves out, and a section if it has to', () => {
    const run = override(file, [{ setting: 'output.trajectory_stride', value: '100' }]);
    expect(numeric(run, 'output', 'trajectory_stride')).toBe(100);
  });

  it('leaves the configuration it was given alone', () => {
    override(file, [{ setting: 'run.steps', value: '2' }]);
    expect(numeric(file, 'run', 'steps')).toBe(100);
  });

  it('refuses a setting that names no section, as the command line does', () => {
    expect(() => override(file, [{ setting: 'steps', value: '2' }])).toThrow(
      ConfigurationError,
    );
  });
});
