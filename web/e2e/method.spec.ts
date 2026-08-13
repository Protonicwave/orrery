import AxeBuilder from '@axe-core/playwright';
import { expect, test } from '@playwright/test';

/**
 * The reading half, in a browser.
 *
 * What is checked here is what only a browser can answer: that a method page
 * runs no script, fetches nothing from anywhere else, is reachable and audited
 * clean, and paints its largest element quickly enough that it can be read
 * before anything else has happened. The unit tests cover what these pages say
 * and where each figure came from.
 */

const ORIGIN = 'http://localhost:4173';

/** Every page of the reading half, by the address it is served at. */
const PAGES = [
  ['the contents', './method/'],
  ['the demonstration', './method/demonstration/'],
  ['validation', './method/validation/'],
  ['performance', './method/performance/'],
  ['the solvers', './method/solvers/'],
] as const;

for (const [name, address] of PAGES) {
  test(`${name} runs no script`, async ({ page }) => {
    const scripts: string[] = [];
    page.on('request', (request) => {
      if (request.resourceType() === 'script') scripts.push(request.url());
    });

    await page.goto(address);
    await page.waitForLoadState('networkidle');

    expect(scripts).toEqual([]);
    // Nothing in the document either, so the page is readable by a reader who
    // runs no script at all rather than merely by one who waits for it.
    expect(await page.evaluate(() => document.scripts.length)).toBe(0);
  });

  test(`${name} fetches nothing from another origin`, async ({ page }) => {
    const foreign: string[] = [];
    page.on('request', (request) => {
      if (new URL(request.url()).origin !== ORIGIN) foreign.push(request.url());
    });

    await page.goto(address);
    await page.waitForLoadState('networkidle');

    expect(foreign).toEqual([]);
  });

  test(`${name} passes an accessibility audit`, async ({ page }) => {
    await page.goto(address);
    const results = await new AxeBuilder({ page })
      .withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa', 'wcag22aa'])
      .analyze();

    expect(results.violations).toEqual([]);
  });
}

test('paints a method page before a second has passed', async ({ page }) => {
  await page.goto('./method/performance/');

  // The largest contentful paint, as the browser measures it rather than as a
  // wall clock outside it measures the navigation. The budget is a second, and
  // a page of markup and one stylesheet served from the same origin is far
  // inside it; this fails if the reading half acquires something that blocks.
  const painted = await page.evaluate(
    () =>
      new Promise<number>((resolve) => {
        new PerformanceObserver((list) => {
          const entries = list.getEntries();
          const last = entries[entries.length - 1];
          if (last !== undefined) resolve(last.startTime);
        }).observe({ type: 'largest-contentful-paint', buffered: true });
        setTimeout(() => resolve(Number.POSITIVE_INFINITY), 4000);
      }),
  );

  expect(painted).toBeLessThan(1000);
});

test('moves nothing as its fonts arrive', async ({ page }) => {
  await page.goto('./method/validation/');

  const shifted = await page.evaluate(
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
        setTimeout(() => resolve(total), 2500);
      }),
  );

  // Nothing on a method page is learned from the network, so there is no field
  // whose contents arrive after the box around them has been drawn, and on a
  // machine that is not busy the score is exactly zero.
  //
  // It is not always exactly zero, and the reason is the one thing on these
  // pages that does arrive: the faces. They are preloaded and served from this
  // origin, so the swap window is normally over before the first paint, but on
  // a runner slow enough to paint the fallback first the swap reflows the prose
  // and the metric records it. The measured score when that happens is about
  // two parts in a hundred thousand. The bound is well inside the threshold at
  // which the metric calls a page good and well outside that, so a page that
  // starts genuinely moving fails.
  expect(shifted).toBeLessThan(0.001);
});

test('goes back to the instrument and on to the next page', async ({ page }) => {
  await page.goto('./method/demonstration/');

  await page.getByRole('link', { name: 'Instrument', exact: true }).click();
  await expect(page).toHaveURL(/\/orrery\/instrument\/$/);

  await page.goto('./method/demonstration/');
  await page.getByRole('link', { name: 'play this run' }).click();
  await expect(page).toHaveURL(/run=collision/);
});

test('reaches the instrument from the instrument', async ({ page }) => {
  await page.goto('./');
  await page.getByRole('link', { name: 'Method', exact: true }).click();
  await expect(page).toHaveURL(/\/method\/$/);
  await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
});

test('draws its focus ring in the brass that carries on paper', async ({ page }) => {
  await page.goto('./method/');
  await page.locator('body').press('Tab');

  const outline = await page.evaluate(() => {
    const active = document.activeElement;
    if (active === null) return null;
    const style = getComputedStyle(active);
    return { colour: style.outlineColor, width: style.outlineWidth };
  });

  // --brass-dim, which is 4.7:1 against the paper where the full brass is 2.4.
  expect(outline?.colour).toBe('rgb(138, 100, 44)');
  expect(outline?.width).toBe('1px');
});
