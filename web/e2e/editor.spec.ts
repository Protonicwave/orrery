import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';

/**
 * The editor, from the outside.
 *
 * The unit tests know that the drawing is where it should be and that the file
 * says what it should say. What only a browser can answer is whether the two are
 * on the page together, whether the drawing follows a setting being changed,
 * whether the file can be taken away, and whether all of it can be reached from
 * a keyboard.
 */

const EDITOR = './editor/';

test('opens on the demonstration, drawn and described', async ({ page }) => {
  await page.goto(EDITOR);

  await expect(page.getByRole('heading', { name: 'Galaxy collision' })).toBeVisible();

  // The drawing carries a description of itself, because a picture of an
  // encounter is neither decoration nor readable.
  const plan = page.getByRole('img', { name: /Two disc galaxies/ });
  await expect(plan).toBeVisible();
  await expect(plan).toHaveAccessibleName(/periapsis/);

  // And the elements beside it are the ones the placement gives.
  await expect(page.getByText('bound · merges')).toBeVisible();
});

test('draws the design again when a setting is changed', async ({ page }) => {
  await page.goto(EDITOR);

  const plan = page.getByRole('img', { name: /Two disc galaxies/ });
  const before = await plan.innerHTML();

  const separation = page.getByRole('slider', { name: 'Separation' });
  await separation.focus();
  for (let press = 0; press < 8; press += 1) await separation.press('ArrowRight');

  await expect(plan).not.toHaveText('');
  expect(await plan.innerHTML()).not.toBe(before);

  // The file follows the drawing, since they are the same design read two ways.
  await expect(page.getByRole('button', { name: /Download/ })).toBeVisible();
  await expect(page.locator('pre')).toContainText('separation = 24');
});

test('walks the four presets, and each one is a file it would offer', async ({
  page,
}) => {
  await page.goto(EDITOR);

  for (const [kind, heading] of [
    ['kepler', 'Kepler two-body'],
    ['plummer', 'Plummer sphere'],
    ['disc-galaxy', 'Single disc'],
    ['galaxy-collision', 'Galaxy collision'],
  ] as const) {
    await page.getByRole('radio', { name: kind, exact: true }).check({ force: true });
    await expect(page.getByRole('heading', { name: heading })).toBeVisible();

    // A design the reader would refuse cannot be downloaded, and none of the
    // presets is one.
    await expect(page.getByRole('button', { name: /Download/ })).toBeEnabled();
    await expect(page.locator('pre')).toContainText(`kind = ${kind}`);
  }
});

test('hands over the file it is showing', async ({ page }) => {
  await page.goto(EDITOR);

  const [download] = await Promise.all([
    page.waitForEvent('download'),
    page.getByRole('button', { name: /Download/ }).click(),
  ]);

  expect(download.suggestedFilename()).toBe('collision-20260812.orrery');

  const stream = await download.createReadStream();
  const chunks: Buffer[] = [];
  for await (const chunk of stream) chunks.push(chunk as Buffer);
  const text = Buffer.concat(chunks).toString('utf8');

  expect(text).toContain('[initial_conditions]');
  expect(text).toContain('kind = galaxy-collision');
  expect(text).toContain('orrery run collision-20260812.orrery');
  expect(text).toBe(await page.locator('pre').innerText());
});

test('reaches the drawing and every control from the keyboard', async ({ page }) => {
  await page.goto(EDITOR);

  // The skip link first, as on the instrument.
  await page.keyboard.press('Tab');
  await expect(page.getByRole('link', { name: 'Skip to the drawing' })).toBeFocused();

  // The handle on the drawing is operable by the arrow keys as well as by a
  // pointer, so a design can be laid out without one.
  const handle = page.getByRole('button', { name: /Secondary galaxy/ });
  await handle.focus();
  await expect(handle).toBeFocused();

  const before = await page.locator('pre').innerText();
  await handle.press('ArrowRight');
  await handle.press('ArrowRight');
  expect(await page.locator('pre').innerText()).not.toBe(before);
});

test('passes an audit of the rendered page', async ({ page }) => {
  await page.goto(EDITOR);
  await expect(page.getByRole('heading', { name: 'Galaxy collision' })).toBeVisible();

  const results = await new AxeBuilder({ page })
    .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
    .analyze();

  expect(results.violations).toEqual([]);
});

test('steps the design it is showing, and says what stepped it', async ({ page }) => {
  await page.goto(EDITOR);

  await page.getByRole('button', { name: /Sample and step it here/ }).click();

  // The module has to be fetched and instantiated before anything is sampled,
  // so this waits for what the run reports rather than for a length of time.
  await expect(page.getByText('barnes-hut, scalar, 1 thread')).toBeVisible({
    timeout: 30_000,
  });
  await expect(page.getByRole('button', { name: /Stop the preview/ })).toBeVisible();
});
