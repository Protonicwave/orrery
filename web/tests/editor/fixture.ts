/**
 * The design the round-trip test runs.
 *
 * The editor's claim is that a design leaves it as a configuration the native
 * binary runs unmodified. Testing that needs one design written down in one
 * place: this is it, and both halves of the test read it from here, so the file
 * committed in `tests/fixtures/` and the design compared against it cannot come
 * apart.
 */

import { type Design, preset } from '../../src/editor/design';

/**
 * A collision small enough to run twice in a test.
 *
 * Two thousand particles rather than sixty thousand, and two hundred steps
 * rather than forty thousand. Everything else is the demonstration's, because
 * what is being tested is that the two builds agree about a configuration the
 * editor writes, and one nobody would ever write would be a weaker thing to
 * agree about.
 */
export const FIXTURE_DESIGN: Design = {
  ...preset('galaxy-collision'),
  count: 2000,
  steps: 200,
  seed: 20260814,
};
