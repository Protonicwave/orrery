import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { describe, expect, it } from 'vitest';

/**
 * Every figure on a method page, traced to the file it came from.
 *
 * The reading half is prose, so a number in it is typed rather than read out
 * of anything at run time, and a typed number is one that can be wrong. Each
 * one therefore names its source in the markup, and this test opens that file
 * and requires the figure to be in it:
 *
 *     <span class="num" data-source="docs/performance.md">95.68</span>
 *
 * Where the page sets a figure differently from the way the report writes it,
 * which is usually a power of ten, the page also states what the report says:
 *
 *     <td class="n" data-says="2.6894e-3">2.6894×10<sup>−3</sup></td>
 *
 * A table states its source once and every figure cell in it is checked, so a
 * results table cannot be transcribed from one report and attributed to
 * another.
 */

const WEB = resolve(import.meta.dirname, '../..');
const ROOT = resolve(WEB, '..');
const PAGES = resolve(WEB, 'method');

/** Every method page, found rather than listed, so a new one is covered. */
function pages(directory: string): string[] {
  const found: string[] = [];
  for (const entry of readdirSync(directory)) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) found.push(...pages(path));
    else if (entry.endsWith('.html')) found.push(path);
  }
  return found;
}

/**
 * The form of a figure that two files can be compared in.
 *
 * The pages group digits with a thin space, set a minus as a minus sign rather
 * than as a hyphen, and wrap a sentence wherever it reaches the margin. A
 * report does none of those things, and none of them is a difference in the
 * figure.
 */
function plain(text: string): string {
  return text.replace(/[   ]/g, ' ').replace(/−/g, '-').replace(/\s+/g, ' ').trim();
}

/** A figure, and the file it is claimed to come from. */
interface Claim {
  readonly figure: string;
  readonly source: string;
}

/** What one page claims, read out of its markup. */
function claims(html: string): Claim[] {
  const document = new DOMParser().parseFromString(html, 'text/html');
  const found: Claim[] = [];

  for (const element of document.querySelectorAll('[data-source]')) {
    const source = element.getAttribute('data-source') as string;

    if (element.tagName === 'TABLE') {
      for (const cell of element.querySelectorAll('td.n')) {
        const figure = plain(cell.getAttribute('data-says') ?? cell.textContent);
        // A cell holding no digit is a word: "not run", "Bounded". Those are
        // read by the reviewer of the prose, not by this test.
        if (/\d/.test(figure)) found.push({ figure, source });
      }
      continue;
    }

    found.push({
      figure: plain(element.getAttribute('data-says') ?? element.textContent),
      source,
    });
  }

  return found;
}

const SOURCES = new Map<string, string>();

/** A source file, read once however many figures cite it. */
function source(path: string): string {
  const already = SOURCES.get(path);
  if (already !== undefined) return already;
  const text = plain(readFileSync(resolve(ROOT, path), 'utf8'));
  SOURCES.set(path, text);
  return text;
}

describe.each(pages(PAGES).map((path) => [path.slice(PAGES.length + 1), path]))(
  '%s',
  (_name, path) => {
    const html = readFileSync(path, 'utf8');
    const stated = claims(html);

    // The contents page carries no measurement and is the one page that
    // should not. Every other page states figures, and a page of prose with
    // none on it has stopped being an argument about a measurement; without
    // this it would pass the file below in silence.
    it.skipIf(_name === 'index.html')('states a figure', () => {
      expect(stated.length).toBeGreaterThan(0);
    });

    it.each(stated.map((claim) => [claim.figure, claim.source]))(
      '%s is in %s',
      (figure, path) => {
        expect(source(path)).toContain(figure);
      },
    );

    it('runs no script', () => {
      // The reading half costs no JavaScript to read, which is the whole of
      // ADR-0050 and is a property of the file rather than of the build.
      expect(html).not.toMatch(/<script/i);
    });

    it('sets its language and names itself', () => {
      const document = new DOMParser().parseFromString(html, 'text/html');
      expect(document.documentElement.getAttribute('lang')).toBe('en-GB');
      expect(document.title).not.toBe('');
      expect(document.querySelector('h1')?.textContent?.trim()).not.toBe('');
    });

    it('offers a way back to the instrument', () => {
      const document = new DOMParser().parseFromString(html, 'text/html');
      const nav = document.querySelector('nav.nav');
      expect(nav?.textContent).toContain('Instrument');
    });
  },
);
