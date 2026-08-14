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
 * The editor is a page below it, so it reads the same addresses through
 * `methodFrom`, which is one function rather than a second table.
 */

export const METHOD = {
  contents: './method/',
  demonstration: './method/demonstration/',
  validation: './method/validation/',
  performance: './method/performance/',
  solvers: './method/solvers/',
} as const;

/** The two pages of the client that run a script. */
export type Page = 'instrument' | 'editor';

/** Where each page is, as seen from `from`. */
export function pageFrom(from: Page, to: Page): string {
  if (from === to) return './';
  return to === 'editor' ? './editor/' : '../';
}

/** The reading half's addresses, as seen from `from`. */
export function methodFrom(from: Page): Readonly<Record<keyof typeof METHOD, string>> {
  if (from === 'instrument') return METHOD;

  const raised: Record<string, string> = {};
  for (const [name, address] of Object.entries(METHOD)) {
    raised[name] = address.replace(/^\.\//, '../');
  }
  return raised as Record<keyof typeof METHOD, string>;
}
