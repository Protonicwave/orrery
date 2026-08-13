import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

/**
 * The plate, in a browser that has a graphics device.
 *
 * These are the checks nothing else can make. A unit test can say the reader
 * decodes a frame correctly and a type can say the renderer interface is
 * implemented; only a browser can say that a picture appeared, that both
 * backends drew the same one, and that a minute of playback left the heap
 * where it started.
 *
 * The backend is pinned with `?renderer=`, because a comparison between the two
 * is not a comparison if the machine chooses which one runs. A machine with no
 * WebGPU adapter, which is what continuous integration usually is, skips the
 * comparison rather than failing it: what matters there is that the fallback
 * works, and every other test in this file exercises the fallback.
 */

/** Long enough for the header and the first requests of frames to arrive. */
const READY = 20_000;

/** The instant both backends are compared at, in model time. */
const INSTANT = 20;

async function open(page: Page, backend: string): Promise<void> {
  await page.goto(`./?renderer=${backend}`);
  await expect(page.locator('canvas')).toBeVisible();
  await expect.poll(() => frames(page), { timeout: READY }).toBeGreaterThan(0);
}

/** How many frames have been decoded, which the canvas states for a reader. */
async function frames(page: Page): Promise<number> {
  const label = await page.evaluate(
    () => document.querySelector('canvas')?.getAttribute('aria-label') ?? '',
  );
  return Number(/(\d+) of \d+ frames read/.exec(label)?.[1] ?? 0);
}

/** What the plate says it drew with, which is the backend that was asked for. */
async function device(page: Page): Promise<string> {
  return (await page.getByTitle(/WebGPU|WebGL2/).textContent()) ?? '';
}

/**
 * The picture as a sixteen by sixteen grid of mean brightnesses.
 *
 * Two drivers rasterising the same triangles do not produce the same bytes, and
 * a comparison that demanded they did would fail on the first machine with a
 * different rounding mode. What has to agree is the picture: where the light is
 * and how much of it there is. A coarse grid of means is that statement, and it
 * is unforgiving of the mistakes that actually happen here, an image flipped in
 * y, a sprite size in the wrong units, a tone curve applied twice.
 *
 * The image comes from a screenshot rather than from the canvas in the page.
 * Reading a canvas back needs its drawing buffer to have been preserved, which
 * costs a copy on every frame of a path that must not have one, and a WebGPU
 * canvas will not be drawn into a 2D context at all once it has been presented.
 * A screenshot is what the reader saw, which is the thing being compared.
 */
async function signature(page: Page, shot: Buffer): Promise<number[]> {
  const encoded = shot.toString('base64');
  return page.evaluate(async (data: string) => {
    const image = new Image();
    image.src = `data:image/png;base64,${data}`;
    await image.decode();

    const scratch = document.createElement('canvas');
    scratch.width = 16;
    scratch.height = 16;
    const context = scratch.getContext('2d');
    if (context === null) return [];

    context.drawImage(image, 0, 0, 16, 16);
    const pixels = context.getImageData(0, 0, 16, 16).data;
    const tiles: number[] = [];
    for (let index = 0; index < pixels.length; index += 4) {
      tiles.push(
        ((pixels[index] as number) +
          (pixels[index + 1] as number) +
          (pixels[index + 2] as number)) /
          3,
      );
    }
    return tiles;
  }, encoded);
}

/**
 * Take the annotation off the plate, leaving only the picture.
 *
 * The catalogue in the corners names the device that drew the exposure, and the
 * two devices have different names, so a screenshot that included it would
 * differ between the backends for a reason that has nothing to do with the
 * rendering. What is being compared is the picture.
 */
async function bare(page: Page): Promise<void> {
  await page.evaluate(() => {
    const plate = document.querySelector('#plate');
    for (const child of plate?.children ?? []) {
      if (!(child instanceof HTMLElement) && !(child instanceof SVGElement)) continue;
      if (child.querySelector('canvas') !== null) continue;
      child.style.visibility = 'hidden';
    }
  });
}

/** Put both viewers on the same instant, so the same frame is being compared. */
async function seek(page: Page, time: number): Promise<void> {
  await expect.poll(() => frames(page), { timeout: READY }).toBeGreaterThan(50);
  await page.getByRole('slider', { name: 'Position in the run' }).fill(String(time));
  await expect(page.locator('canvas')).toBeVisible();
  await page.waitForTimeout(500);
}

test('draws the published run', async ({ page }) => {
  await open(page, 'webgl2');
  expect(await device(page)).toContain('WebGL2');

  // A plate with a picture on it is not a plate saying it has none.
  await expect(page.getByText('The plate is being exposed')).toBeHidden();

  // The catalogue states the run that was drawn, and the count on it comes from
  // the trajectory's own header rather than from a configuration file.
  await expect(page.getByText('8 000').first()).toBeVisible();
});

test('plays, and stops where it is paused', async ({ page }) => {
  await open(page, 'webgl2');

  const clock = page.locator('p').filter({ hasText: '/ 200.000' }).first();
  await expect(clock).toContainText('0.000');

  await page.getByRole('button', { name: 'Play' }).click();
  await expect(page.getByRole('button', { name: 'Pause' })).toBeVisible();
  await expect
    .poll(async () => (await clock.textContent()) ?? '')
    .not.toMatch(/^0\.000/);

  await page.getByRole('button', { name: 'Pause' }).click();

  // The clock is sampled from the render loop ten times a second, so the
  // reading taken the instant the button is released is one the loop has not
  // caught up with yet. Settle first, then require it to stay where it is.
  await page.waitForTimeout(400);
  const stopped = await clock.textContent();
  await page.waitForTimeout(600);
  expect(await clock.textContent()).toBe(stopped);
});

test('takes the camera from the keyboard as well as the mouse', async ({ page }) => {
  await open(page, 'webgl2');

  const canvas = page.locator('canvas');
  await canvas.focus();
  await expect(canvas).toBeFocused();

  // The scale bar is the camera's own readout of how far away it is, so a zoom
  // that did nothing would leave it where it was.
  const bar = page.locator('[class*="scaleBar"]');
  const before = await bar.evaluate((element) => element.clientWidth);
  for (let press = 0; press < 12; press += 1) await page.keyboard.press('PageDown');
  await expect
    .poll(() => bar.evaluate((element) => element.clientWidth))
    .not.toBe(before);
});

test('draws the same picture through either backend', async ({ page, browser }) => {
  // The same viewport as the first page, and not the default. The camera frames
  // the particles by their extent and projects them through the aspect ratio of
  // the plate, so two windows of different shapes are two different pictures of
  // the same run and would fail this comparison for the wrong reason.
  const size = page.viewportSize();
  const other = await browser.newPage(size === null ? {} : { viewport: size });
  await open(other, 'webgpu');
  const named = await device(other);
  test.skip(!named.includes('WebGPU'), 'this machine offers no WebGPU adapter');

  await open(page, 'webgl2');
  await seek(page, INSTANT);
  await seek(other, INSTANT);

  await bare(page);
  await bare(other);
  const fallback = await signature(page, await page.locator('canvas').screenshot());
  const primary = await signature(other, await other.locator('canvas').screenshot());
  await other.close();

  expect(fallback).toHaveLength(256);
  expect(primary).toHaveLength(256);

  // Something is lit, so this is not two black rectangles agreeing, and the
  // light is where the merger is rather than spread over the whole plate.
  expect(Math.max(...fallback)).toBeGreaterThan(32);
  expect(Math.max(...primary)).toBeGreaterThan(32);

  for (let tile = 0; tile < 256; tile += 1) {
    expect(
      Math.abs((primary[tile] as number) - (fallback[tile] as number)),
      `tile ${tile} of the picture`,
    ).toBeLessThan(8);
  }
});
