import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';

/**
 * What is checked here is what only a browser can answer: that nothing is
 * fetched from anywhere else, that the layout does not move as the fonts
 * arrive, that a keyboard reaches every control, and that an audit of the
 * rendered page finds nothing. The unit tests cover what the interface says;
 * these cover what it is like to use.
 */

/** Where the client is served from during these tests. */
const ORIGIN = 'http://localhost:4173';

test('fetches nothing from another origin', async ({ page }) => {
  const foreign: string[] = [];
  page.on('request', (request) => {
    if (new URL(request.url()).origin !== ORIGIN) foreign.push(request.url());
  });

  await page.goto('./');
  await page.waitForLoadState('networkidle');

  expect(foreign).toEqual([]);
});

test('sets its type in the faces it ships', async ({ page }) => {
  await page.goto('./');
  await page.evaluate(() => document.fonts.ready);

  const faces = await page.evaluate(() =>
    [...document.fonts]
      .filter((face) => face.status === 'loaded')
      .map((face) => face.family),
  );
  expect(faces).toContain('IBM Plex Sans');
  expect(faces).toContain('IBM Plex Mono');
});

test('moves nothing as it loads', async ({ page }) => {
  await page.goto('./');
  const shift = await page.evaluate(
    () =>
      new Promise<number>((resolve) => {
        let total = 0;
        new PerformanceObserver((list) => {
          for (const entry of list.getEntries()) {
            const layout = entry as PerformanceEntry & {
              value: number;
              hadRecentInput: boolean;
            };
            if (!layout.hadRecentInput) total += layout.value;
          }
        }).observe({ type: 'layout-shift', buffered: true });
        setTimeout(() => resolve(total), 1000);
      }),
  );

  // The budget is zero, not a small number. Tabular figures and preloaded
  // faces are what make that reachable rather than lucky.
  expect(shift).toBe(0);
});

test('reaches every control with the keyboard', async ({ page }) => {
  await page.goto('./');
  await page.locator('body').press('Tab');

  const reached: string[] = [];
  for (let stop = 0; stop < 24; stop += 1) {
    const description = await page.evaluate(() => {
      const active = document.activeElement;
      if (active === null || active === document.body) return null;
      const name =
        active.getAttribute('aria-label') ?? active.textContent?.trim() ?? '';
      return `${active.tagName.toLowerCase()}:${name.slice(0, 24)}`;
    });
    if (description === null) break;
    reached.push(description);
    await page.keyboard.press('Tab');
  }

  // The skip link first, then the navigation, then every control in the order
  // the instrument is laid out in.
  expect(reached[0]).toContain('Skip to the instrument');
  expect(
    reached.filter((stop) => stop.startsWith('input')).length,
  ).toBeGreaterThanOrEqual(7);
  expect(reached.some((stop) => stop.includes('Trails'))).toBe(true);
  expect(reached.some((stop) => stop.includes('Recompute'))).toBe(true);
});

test('draws a focus ring of its own rather than the browser default', async ({
  page,
}) => {
  await page.goto('./');
  await page.locator('body').press('Tab');

  const outline = await page.evaluate(() => {
    const active = document.activeElement;
    if (active === null) return null;
    const style = getComputedStyle(active);
    return { colour: style.outlineColor, width: style.outlineWidth };
  });

  expect(outline?.colour).toBe('rgb(200, 145, 63)');
  expect(outline?.width).toBe('1px');
});

test('passes an accessibility audit', async ({ page }) => {
  await page.goto('./');
  const results = await new AxeBuilder({ page })
    .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
    .analyze();

  expect(results.violations).toEqual([]);
});
