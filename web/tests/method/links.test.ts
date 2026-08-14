import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { describe, expect, it } from 'vitest';
import { METHOD, methodFrom, pageFrom } from '../../src/method/links';

/**
 * The instrument's links into the reading half, and the reading half's back.
 *
 * Nothing imports a static page, so a page that was renamed would leave a dead
 * link in the instrument and a build that still succeeded. These two checks are
 * what stands in for the import.
 */

const WEB = resolve(import.meta.dirname, '../..');

describe('the reading half', () => {
  it.each(Object.entries(METHOD))(
    'has the page the instrument links to as %s',
    (_name, address) => {
      // './method/validation/' is served by 'web/method/validation/index.html',
      // which is the file the build takes as that page's entry.
      const page = resolve(WEB, address.replace(/^\.\//, ''), 'index.html');
      expect(existsSync(page)).toBe(true);
    },
  );

  it('is an entry of the build for every page it has', async () => {
    const configuration = await import('../../vite.config.ts');
    const input = configuration.default.build?.rollupOptions?.input as Record<
      string,
      string
    >;
    const entries = Object.values(input).map((path) => path.replace(/\\/g, '/'));

    for (const address of Object.values(METHOD)) {
      const page = `${address.replace(/^\.\//, '')}index.html`;
      expect(entries.some((entry) => entry.endsWith(page))).toBe(true);
    }

    // The editor is a page of the same site and is built as its own entry, so
    // the same check applies to it.
    expect(entries.some((entry) => entry.endsWith('editor/index.html'))).toBe(true);
  });
});

describe('the two pages that run a script', () => {
  it('address the reading half from where each of them is', () => {
    // The editor sits one directory below the instrument, so the addresses that
    // are correct from one are wrong from the other by exactly one level.
    expect(methodFrom('instrument')).toEqual(METHOD);
    expect(methodFrom('editor').validation).toBe('../method/validation/');
    expect(Object.keys(methodFrom('editor'))).toEqual(Object.keys(METHOD));
  });

  it('address each other', () => {
    expect(pageFrom('instrument', 'editor')).toBe('./editor/');
    expect(pageFrom('editor', 'instrument')).toBe('../');
    expect(pageFrom('editor', 'editor')).toBe('./');
    expect(pageFrom('instrument', 'instrument')).toBe('./');
  });
});
