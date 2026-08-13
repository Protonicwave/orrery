/**
 * A reader for the configuration format, to the specification in
 * docs/formats/configuration.md.
 *
 * The client parses the repository's own configuration files rather than
 * holding a copy of what is in them. A figure shown in the data rail is
 * therefore the figure the run was given, and the two cannot drift apart: if
 * examples/collision.orrery changes, the interface changes with it.
 *
 * This is the reading half of the format only. It is strict in the ways the
 * C++ reader is strict, because a file it accepts and the C++ rejects would be
 * worse than no reader at all, but it does not know the settings' names or
 * their defaults. That knowledge belongs to whatever asks for a value.
 */

/** A parsed file: sections, each a set of settings, in the order they appear. */
export type Configuration = Readonly<Record<string, Readonly<Record<string, string>>>>;

/** Everything wrong with a file is reported the way the C++ reads it: by line. */
export class ConfigurationError extends Error {
  readonly line: number;

  constructor(line: number, message: string) {
    super(`line ${line}: ${message}`);
    this.name = 'ConfigurationError';
    this.line = line;
  }
}

const SECTION = /^\[([A-Za-z0-9_]+)\]$/;

/**
 * Parse the text of a configuration file.
 *
 * @throws ConfigurationError on a line that the C++ reader would reject: an
 * unparseable line, a setting with no value, a setting outside any section
 * that does not name its own, or the same setting given twice.
 */
export function parseConfiguration(text: string): Configuration {
  const sections: Record<string, Record<string, string>> = {};
  let current: string | undefined;

  const lines = text.split(/\r?\n/);
  for (const [index, raw] of lines.entries()) {
    const number = index + 1;
    const line = raw.trim();
    if (line === '' || line.startsWith('#')) continue;

    const heading = SECTION.exec(line);
    if (heading?.[1] !== undefined) {
      current = heading[1];
      sections[current] ??= {};
      continue;
    }

    const equals = line.indexOf('=');
    if (equals < 0) {
      throw new ConfigurationError(number, `expected a setting, read "${line}"`);
    }

    const name = line.slice(0, equals).trim();
    const value = line.slice(equals + 1).trim();
    if (name === '') throw new ConfigurationError(number, 'a setting with no name');
    if (value === '') throw new ConfigurationError(number, `${name} has no value`);

    // A key may name its own section, and doing so does not change the section
    // the following lines belong to.
    const dot = name.indexOf('.');
    const section = dot < 0 ? current : name.slice(0, dot);
    const key = dot < 0 ? name : name.slice(dot + 1);
    if (section === undefined) {
      throw new ConfigurationError(number, `${name} is outside any section`);
    }

    sections[section] ??= {};
    const settings = sections[section];
    if (key in settings) {
      throw new ConfigurationError(number, `${section}.${key} is set twice`);
    }
    settings[key] = value;
  }

  return sections;
}

/**
 * A configuration with some settings replaced, as `--set` replaces them.
 *
 * The published gallery runs are the repository's own configurations with a
 * few settings changed, and the interface has to describe the run that was
 * actually made rather than the file it started from. Applying the overrides
 * here, once, is what keeps every panel agreeing: a particle count on the plate
 * and a particle count in the data rail are then the same number arrived at the
 * same way.
 *
 * A setting that names no section is an error, as it is on the command line.
 */
export function override(
  configuration: Configuration,
  settings: readonly { readonly setting: string; readonly value: string }[],
): Configuration {
  const result: Record<string, Record<string, string>> = {};
  for (const [section, values] of Object.entries(configuration)) {
    result[section] = { ...values };
  }

  for (const { setting: name, value } of settings) {
    const dot = name.indexOf('.');
    if (dot < 0) {
      throw new ConfigurationError(0, `${name} does not name a section`);
    }
    const section = name.slice(0, dot);
    result[section] ??= {};
    (result[section] as Record<string, string>)[name.slice(dot + 1)] = value;
  }

  return result;
}

/**
 * Write a configuration back out in the format above.
 *
 * The writing half exists for one caller: the WebAssembly module takes the text
 * of a configuration rather than a structure of numbers, so that the browser and
 * the command line read the same document with the same parser. Something has to
 * turn an edited configuration back into that document, and doing it here keeps
 * the reader and the writer in one file where a change to either is visible to
 * whoever makes it.
 *
 * Sections and settings are written in the order they were read, and a
 * configuration built by `override` keeps that order, so a file that goes
 * through this and back comes out as it went in but for what was overridden.
 * Nothing is quoted or escaped, because the format has no quoting: a value is
 * whatever follows the equals sign as far as the end of the line.
 */
export function writeConfiguration(configuration: Configuration): string {
  const lines: string[] = [];
  for (const [section, settings] of Object.entries(configuration)) {
    lines.push(`[${section}]`);
    for (const [key, value] of Object.entries(settings)) {
      lines.push(`${key} = ${value}`);
    }
    lines.push('');
  }
  return lines.join('\n');
}

/** A setting as text, or undefined if the file leaves it to its default. */
export function setting(
  configuration: Configuration,
  section: string,
  key: string,
): string | undefined {
  return configuration[section]?.[key];
}

/**
 * A setting as a number.
 *
 * @throws ConfigurationError if the file states it and it does not parse, which
 * is the case the C++ reader is strictest about: a value with anything after
 * it is an error rather than a prefix.
 */
export function numeric(
  configuration: Configuration,
  section: string,
  key: string,
): number | undefined {
  const text = setting(configuration, section, key);
  if (text === undefined) return undefined;
  if (!/^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$/.test(text)) {
    throw new ConfigurationError(0, `${section}.${key} is not a number: "${text}"`);
  }
  return Number(text);
}

/**
 * A short, stable name for a configuration: the seed it samples from and four
 * hexadecimal digits of a hash over the file itself.
 *
 * A run has to be identifiable on the plate, and a name invented for the
 * purpose would be a fact the repository cannot reproduce. This one is a
 * function of the file, so the same file always earns the same name and a
 * changed file earns a different one. The hash is FNV-1a, which is chosen for
 * being four lines rather than for its collision behaviour; nothing depends on
 * two configurations that differ being unable to share a name.
 */
export function identifier(text: string, seed: number): string {
  let hash = 0x811c9dc5;
  for (let index = 0; index < text.length; index += 1) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  const digits = (hash >>> 16).toString(16).toUpperCase().padStart(4, '0');
  return `${seed}-${digits}`;
}
