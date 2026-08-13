import type { Page } from '@playwright/test';
import { expect, test } from '@playwright/test';

/**
 * The gallery, and the address that names a run and a moment in it.
 *
 * What these check is that a run is a place: that it can be arrived at, moved
 * away from, gone back to, and sent to somebody. Whether the run then draws
 * correctly is `plate.spec.ts`, and what its figures are is the unit tests'.
 *
 * The backend is pinned to WebGL2, which every machine that runs these has,
 * because none of this is about which device drew the picture.
 */

const READY = 20_000;

/** How much of the run has been read, which the plate states for a reader. */
async function frames(page: Page): Promise<number> {
  const label = await page.evaluate(
    () => document.querySelector('#plate canvas')?.getAttribute('aria-label') ?? '',
  );
  return Number(/(\d+) of \d+ frames read/.exec(label)?.[1] ?? 0);
}

/** The clock, in model time. */
async function clock(page: Page): Promise<number> {
  const shown = await page.getByRole('slider', { name: 'Position in the run' });
  return Number(await shown.inputValue());
}

async function open(page: Page, query: string): Promise<void> {
  await page.goto(`./?renderer=webgl2${query}`);
  await expect.poll(() => frames(page), { timeout: READY }).toBeGreaterThan(0);
}

test('opens the demonstration when the address names no run', async ({ page }) => {
  await open(page, '');
  await expect(page.getByText('examples/collision.orrery')).toBeVisible();
});

test('loads a run into the instrument, and says which is loaded', async ({ page }) => {
  await open(page, '&run=kepler');

  await expect(page.getByText('examples/kepler.orrery')).toBeVisible();
  // The plate states the count out of the trajectory's own header, so this is
  // the file having been read rather than the configuration having been parsed.
  await expect
    .poll(async () => (await page.getByRole('img').getAttribute('aria-label')) ?? '')
    .toContain('2 particles');

  // Marked as the current page, which is how the strip says which of three is
  // loaded to something that cannot see that one of them is brass.
  const gallery = page.getByRole('navigation', { name: 'Published runs' });
  const chosen = gallery.locator('a[aria-current="page"]');
  await expect(chosen).toHaveCount(1);
  await expect(chosen).toContainText('Two bodies on an eccentric orbit');
});

test('plots the run’s own diagnostics beside it', async ({ page }) => {
  await open(page, '&run=cluster');

  // A Plummer sphere is a self-consistent equilibrium, so a correct sample has
  // a virial ratio of one and keeps it. Reading it off the rail is what says
  // the plots belong to the run that is loaded rather than to another.
  const rail = page.getByRole('complementary', { name: 'Run data' });
  await expect(rail.getByText('Virial ratio', { exact: true })).toBeVisible();
  await expect
    .poll(async () => (await rail.textContent()) ?? '')
    .toMatch(/samples\s*101/);
});

test('carries a moment in the address, and arrives at it', async ({ page }) => {
  await open(page, '&run=cluster&t=12');

  // The address names a moment and the instrument goes to the frame the run
  // recorded nearest it, which for this run's stride is that moment exactly.
  await expect.poll(() => clock(page), { timeout: READY }).toBeCloseTo(12, 3);
});

test('writes the moment into the address when the transport is moved', async ({
  page,
}) => {
  await open(page, '&run=cluster');
  await expect.poll(() => frames(page), { timeout: READY }).toBeGreaterThan(120);

  await page.getByRole('slider', { name: 'Position in the run' }).fill('5');
  await expect.poll(() => new URL(page.url()).searchParams.get('t')).toBe('5.000');
  await expect.poll(() => new URL(page.url()).searchParams.get('run')).toBe('cluster');
});

test('switches run without reloading, and goes back', async ({ page }) => {
  await open(page, '');

  await page.getByRole('link', { name: /Plummer sphere/ }).click();
  await expect.poll(() => new URL(page.url()).searchParams.get('run')).toBe('cluster');
  await expect(page.getByText('examples/cluster.orrery')).toBeVisible();
  await expect.poll(() => frames(page), { timeout: READY }).toBeGreaterThan(0);

  await page.goBack();
  await expect(page.getByText('examples/collision.orrery')).toBeVisible();
  await expect.poll(() => frames(page), { timeout: READY }).toBeGreaterThan(0);
});

/**
 * The link is a link. Choosing a run is intercepted so the client is not
 * fetched again, and the thing being intercepted has to be an ordinary
 * navigation to an address that works on its own.
 */
test('offers each run as an address that works on its own', async ({ page }) => {
  await open(page, '');
  const kepler = page.getByRole('link', { name: /Two bodies/ });
  expect(await kepler.getAttribute('href')).toBe('?run=kepler');
});
