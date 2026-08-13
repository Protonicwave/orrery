import { describe, expect, it } from 'vitest';
import {
  ConfigurationError,
  identifier,
  numeric,
  parseConfiguration,
  setting,
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
