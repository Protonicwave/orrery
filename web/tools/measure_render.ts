/**
 * What a minute of playback costs.
 *
 *     npm run build && npm run preview &
 *     node tools/measure_render.ts
 *
 * Two numbers, and the second is the one this exists for. The frame rate says
 * the picture is being drawn at the rate it claims. The heap says the render
 * loop is not allocating: the budget for that path is no allocation at all, and
 * a loop that allocates a little per frame is indistinguishable from one that
 * does not until it has been running for a while, which is why this runs for a
 * minute rather than for a second.
 *
 * The whole run is read first, and the heap is measured after that has
 * finished. The trajectory is thirty-nine megabytes of Float32Array and it is
 * meant to be on the heap; what is being looked for is growth after it is all
 * there and the loop is doing nothing but drawing frames it already has.
 *
 * Not a test. It takes a minute, it wants a machine with a graphics device, and
 * what it produces is a measurement to be written down rather than a threshold
 * to be passed. `docs/instrument.md` quotes what it reported.
 */

import { chromium } from '@playwright/test';

const URL = process.env.ORRERY_URL ?? 'http://localhost:4173/orrery/instrument/';

/** How long to play for, in milliseconds. */
const MINUTE = 60_000;

/** How long to wait for the whole run to be read before starting. */
const LOAD = 120_000;

interface Sample {
  readonly heap: number;
  readonly frames: number;
}

async function main(): Promise<void> {
  const backend = process.argv[2] ?? 'webgpu';
  const browser = await chromium.launch({ headless: false });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  await page.goto(`${URL}?renderer=${backend}`);

  const client = await page.context().newCDPSession(page);

  // Count every animation frame the page draws, from the page rather than from
  // the harness: the harness cannot see a frame, and a frame rate measured by
  // polling is a measurement of the polling.
  await page.evaluate(() => {
    const counter = { frames: 0 };
    (window as unknown as { orreryFrames: { frames: number } }).orreryFrames = counter;
    const tick = () => {
      counter.frames += 1;
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  });

  const read = async (): Promise<Sample> => {
    await client.send('HeapProfiler.collectGarbage');
    const usage = (await client.send('Runtime.getHeapUsage')) as { usedSize: number };
    const frames = await page.evaluate(
      () =>
        (window as unknown as { orreryFrames: { frames: number } }).orreryFrames.frames,
    );
    return { heap: usage.usedSize, frames };
  };

  const label = async (): Promise<string> =>
    (await page.evaluate(
      () => document.querySelector('canvas')?.getAttribute('aria-label') ?? '',
    )) ?? '';

  const started = Date.now();
  while (Date.now() - started < LOAD) {
    const read = /(\d+) of (\d+) frames read/.exec(await label());
    if (read !== null && read[1] === read[2] && read[1] !== '0') break;
    await page.waitForTimeout(1000);
  }
  console.log(`read: ${await label()}`);

  // Paused, the loop still measures, uploads, draws and tone maps every frame;
  // what stops is the instant changing, so the chrome that displays it is not
  // re-rendered. Running the measurement both ways is what separates what the
  // render loop allocates from what React allocates around it.
  if (!process.argv.includes('--paused')) {
    await page.getByRole('button', { name: 'Play' }).click();
  }
  await page.waitForTimeout(2000);

  const before = await read();
  await page.waitForTimeout(MINUTE);
  const after = await read();

  const drawn = after.frames - before.frames;
  const growth = after.heap - before.heap;

  const plate = await page.evaluate(() => {
    const canvas = document.querySelector('canvas');
    return canvas === null ? 'none' : `${canvas.width} by ${canvas.height}`;
  });

  console.log(`backend:      ${backend}`);
  console.log(`plate:        ${plate} device pixels`);
  console.log(`frames:       ${drawn} in ${MINUTE / 1000} s`);
  console.log(`frame rate:   ${((drawn * 1000) / MINUTE).toFixed(1)} per second`);
  console.log(`heap before:  ${(before.heap / 1024 / 1024).toFixed(2)} MB`);
  console.log(`heap after:   ${(after.heap / 1024 / 1024).toFixed(2)} MB`);
  console.log(
    `growth:       ${(growth / 1024).toFixed(1)} kB, ` +
      `${(growth / Math.max(drawn, 1)).toFixed(1)} bytes per frame`,
  );

  await browser.close();
}

await main();
