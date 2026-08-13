/**
 * The performance budget, asserted rather than hoped for.
 *
 * A budget that is not checked is a preference. This runs after every build,
 * in continuous integration as well as by hand, and fails the build when a
 * limit is passed. The limits are stated below with the reason each one is
 * where it is.
 *
 *     npm run build && npm run budget
 *
 * Run by Node directly rather than through a bundler, which is why the package
 * asks for a Node that reads TypeScript without being told to.
 */

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';

const DIST = resolve(dirname(fileURLToPath(import.meta.url)), '../dist');

interface Budget {
  readonly what: string;
  /** Bytes. Compared against the gzipped size unless compressed is false. */
  readonly limit: number;
  readonly compressed: boolean;
  readonly why: string;
  readonly matches: (path: string) => boolean;
}

const KILOBYTE = 1024;

/**
 * Where the WebAssembly module is served from.
 *
 * Its loader is a .js file and would otherwise count against the interface's
 * budget, which it should not: the interface is what every reader downloads and
 * the module is what somebody downloads after asking for a run. Two budgets,
 * because they answer to two different people.
 */
const SOLVER = `${sep}solver${sep}`;

const BUDGETS: readonly Budget[] = [
  {
    what: 'JavaScript',
    limit: 150 * KILOBYTE,
    compressed: true,
    why: 'the whole of the interface, over a connection that is not the one it was built on',
    matches: (path) => extname(path) === '.js' && !path.includes(SOLVER),
  },
  {
    what: 'WebAssembly',
    limit: 400 * KILOBYTE,
    compressed: true,
    why: 'the solver is a download somebody asked for, but it is still a download; the loader is counted with it because nothing can use one without the other',
    matches: (path) => path.includes(SOLVER),
  },
  {
    what: 'CSS',
    limit: 20 * KILOBYTE,
    compressed: true,
    why: 'one token file and a module per component is a small sheet, and a large one would mean the tokens had stopped being read',
    matches: (path) => extname(path) === '.css',
  },
  {
    what: 'fonts',
    limit: 100 * KILOBYTE,
    compressed: false,
    why: 'seven faces cut to the characters the design sets; a complete face is ten times this on its own',
    matches: (path) => extname(path) === '.woff2',
  },
];

function* files(directory: string): Generator<string> {
  for (const entry of readdirSync(directory)) {
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) yield* files(path);
    else yield path;
  }
}

/**
 * The reading half's budget, which is zero.
 *
 * A method page is markup and a stylesheet and nothing else, so that it can be
 * read before any script has run and by a reader who runs none (ADR-0050).
 * Nothing in the client imports those pages, so the only thing that would put a
 * script tag into one of them is a change to how the site is built, which is
 * exactly the change worth failing a pull request over.
 */
function readingHalfRunsNoScript(): boolean {
  const pages = [...files(join(DIST, 'method'))].filter(
    (path) => extname(path) === '.html',
  );

  if (pages.length === 0) {
    console.error('the reading half is not in the build at all');
    return false;
  }

  let clean = true;
  for (const path of pages) {
    if (!/<script/i.test(readFileSync(path, 'utf8'))) continue;
    console.error(`${path} carries a script and a method page may not`);
    clean = false;
  }
  console.log(`method       ${String(pages.length).padStart(6)} pages, no script`);
  return clean;
}

function main(): void {
  const sizes = new Map<string, number>();
  for (const path of files(DIST)) {
    const contents = readFileSync(path);
    for (const budget of BUDGETS) {
      if (!budget.matches(path)) continue;
      const size = budget.compressed ? gzipSync(contents).length : contents.length;
      sizes.set(budget.what, (sizes.get(budget.what) ?? 0) + size);
    }
  }

  let passed = true;
  for (const budget of BUDGETS) {
    const size = sizes.get(budget.what) ?? 0;
    const share = Math.round((size / budget.limit) * 100);
    const units = budget.compressed ? 'kB gzipped' : 'kB';
    const verdict = size <= budget.limit ? 'within' : 'OVER';
    passed &&= size <= budget.limit;
    console.log(
      `${budget.what.padEnd(11)} ${(size / KILOBYTE).toFixed(1).padStart(6)} ${units}` +
        ` of ${(budget.limit / KILOBYTE).toFixed(0)}  ${share}% ${verdict}`,
    );
    if (size > budget.limit) console.log(`  the budget is there for ${budget.why}`);
  }

  passed &&= readingHalfRunsNoScript();

  if (!passed) {
    console.error('\nA budget has been passed. Either the code comes back down or the');
    console.error('budget moves, and moving it is a decision worth arguing for in the');
    console.error('pull request that moves it.');
    process.exit(1);
  }
}

main();
