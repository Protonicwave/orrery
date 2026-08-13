/**
 * Produce the gallery's trajectories by running the simulator.
 *
 *     node tools/publish_gallery.ts path/to/orrery
 *
 * The published runs are not committed. A trajectory is tens of megabytes and
 * is reproducible from a configuration file and a seed, which is exactly the
 * rule `.gitignore` already states for `*.otj`, and a repository that carries
 * its own output stops being a repository of the thing that produced it. So the
 * gallery is generated: here by hand while working on the client, and in the
 * site workflow before the client is built.
 *
 * Every run comes from `src/gallery/runs.ts` and from the configuration file it
 * names. Nothing about a published run is written down twice, and nothing about
 * one is written down here.
 */

import { spawnSync } from 'node:child_process';
import { mkdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { GALLERY, type GalleryRun } from '../src/gallery/runs.ts';

const WEB = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const ROOT = resolve(WEB, '..');
const OUTPUT = join(WEB, 'public', 'gallery');

function settings(run: GalleryRun, id: string): string[] {
  const overrides = run.overrides.flatMap((override) => [
    '--set',
    `${override.setting}=${override.value}`,
  ]);

  return [
    'run',
    run.configuration,
    ...overrides,
    '--set',
    `output.trajectory_path=${join(OUTPUT, `${id}.otj`)}`,
    '--set',
    `output.diagnostics_path=${join(OUTPUT, `${id}.csv`)}`,
  ];
}

/**
 * Refuse a binary of the wrong precision.
 *
 * `orrery --version` states which it was built in, and getting this wrong is
 * not an error anything else would catch: the run would succeed, the client
 * would read it correctly, and the only symptom would be a download of twice
 * the size. Checked here because the check is one line and the mistake is
 * invisible.
 */
function checkPrecision(binary: string, wanted: string): void {
  const version = spawnSync(binary, ['--version'], { encoding: 'utf8' });
  if (version.status !== 0) {
    throw new Error(`${binary} would not report its version`);
  }
  if (!version.stdout.includes(`${wanted} precision`)) {
    throw new Error(
      `the gallery is published in ${wanted} precision and ${binary} reports: ${version.stdout.trim()}`,
    );
  }
}

function publish(binary: string, run: GalleryRun): void {
  console.log(`${run.id}: ${run.title}`);

  const result = spawnSync(binary, settings(run, run.id), {
    cwd: ROOT,
    stdio: 'inherit',
  });

  if (result.error !== undefined) throw result.error;
  if (result.status !== 0) {
    throw new Error(`${binary} exited ${String(result.status)} on ${run.id}`);
  }

  const bytes = statSync(join(OUTPUT, `${run.id}.otj`)).size;
  console.log(`${run.id}: ${bytes} bytes\n`);
}

function main(): void {
  const binary = process.argv[2];
  if (binary === undefined) {
    console.error('usage: node tools/publish_gallery.ts <path to orrery>');
    console.error('');
    console.error('The binary must come from a single-precision build, which is');
    console.error('what every published run is written in. See src/gallery/runs.ts.');
    process.exit(2);
  }

  // Resolved before anything is spawned. The simulator is run from the
  // repository root, so that the configuration paths in src/gallery/runs.ts are
  // the paths the repository uses, and a relative path to the binary would then
  // be resolved against a directory that is not the one it was typed in.
  const orrery = resolve(binary);

  mkdirSync(OUTPUT, { recursive: true });
  for (const run of GALLERY) {
    checkPrecision(orrery, run.precision);
    publish(orrery, run);
  }
}

main();
