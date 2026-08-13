/**
 * Where the reading half is, stated once.
 *
 * The method pages are static HTML under `web/method/`, built as their own
 * entries of the same site and carrying no script (ADR-0050). Nothing in the
 * client imports them, so a page that was renamed would otherwise leave a dead
 * link behind with nothing to catch it; every link the instrument makes into
 * the reading half is named here, and `tests/method/links.test.ts` requires
 * each one to be a page that exists.
 *
 * The addresses are relative to the instrument, which is the site's base, so
 * they are correct under the base the site is published at and under any other.
 */
export const METHOD = {
  contents: './method/',
  demonstration: './method/demonstration/',
  validation: './method/validation/',
  performance: './method/performance/',
  solvers: './method/solvers/',
} as const;
