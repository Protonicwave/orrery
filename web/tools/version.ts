import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

/**
 * The project's version, read from the one place that holds it.
 *
 * The C++ takes its version from the project() call in CMakeLists.txt, the
 * Python package takes it from the C++, and the client takes it from the same
 * line. A version written down a second time is a version that will disagree
 * with the first one eventually, and the disagreement will be found by a
 * reader rather than by a test.
 */
export function readVersion(): string {
  const text = readFileSync(resolve(HERE, '../../CMakeLists.txt'), 'utf8');
  const version = /^\s*VERSION\s+(\d+\.\d+\.\d+)\s*$/m.exec(text);
  if (version?.[1] === undefined) {
    throw new Error('no VERSION in CMakeLists.txt, so the client cannot state one');
  }
  return version[1];
}
