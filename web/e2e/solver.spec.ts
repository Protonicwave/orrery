import { expect, test } from '@playwright/test';

/**
 * The solver, in a real browser.
 *
 * The unit suite already compares what the module computes against what the
 * native build computed, in Node. What it cannot check is the part that only
 * exists in a browser: that the module instantiates in a Worker, that the
 * frames it posts reach the render loop, that the plate says which of the two
 * kinds of run it is showing, and that a run can be stopped.
 *
 * Kepler is the run these are taken against, because two bodies step in
 * microseconds and the test is about the path a frame takes rather than about
 * how long a step costs.
 */

/** The catalogue entry that says where the picture came from. */
function source(page: import('@playwright/test').Page) {
  return page
    .locator('#plate dt', { hasText: /^SRC$/ })
    .locator('xpath=following-sibling::dd[1]');
}

test('steps a run in this browser and says that is what it did', async ({ page }) => {
  await page.goto('./?run=kepler&renderer=webgl2');

  const plate = page.locator('#plate canvas');
  await expect(plate).toBeVisible();
  await expect(source(page)).toHaveText('published run');

  const start = page.getByRole('button', { name: /Run it in this browser/ });
  await expect(start).toBeVisible();
  await start.click();

  // The catalogue is the whole point: a picture computed here must never be
  // read as one of the figures in docs/performance/.
  await expect(source(page)).toHaveText('this browser', { timeout: 30_000 });

  const solver = page
    .locator('#plate dt', { hasText: /^SOLV$/ })
    .locator('xpath=following-sibling::dd[1]');
  await expect(solver).toContainText('1 thread', { timeout: 30_000 });
  await expect(solver).toContainText('scalar');

  // Frames arriving from the Worker, counted by the text alternative the plate
  // keeps up to date.
  await expect
    .poll(
      async () => {
        const label = await plate.getAttribute('aria-label');
        return Number(/(\d+) of \d+ frames read/.exec(label ?? '')?.[1] ?? 0);
      },
      { timeout: 30_000 },
    )
    .toBeGreaterThan(2);

  // A step time and an energy error, both measured by the module rather than
  // decided by the page.
  const step = page
    .locator('#plate dt', { hasText: /^STEP$/ })
    .locator('xpath=following-sibling::dd[1]');
  await expect(step).toContainText('ms');

  const drift = page
    .locator('#plate dt', { hasText: /^dE\/E$/ })
    .locator('xpath=following-sibling::dd[1]');
  await expect(drift).not.toHaveText('pending', { timeout: 30_000 });
});

test('stops a browser run and goes back to the published one', async ({ page }) => {
  await page.goto('./?run=kepler&renderer=webgl2');

  await page.getByRole('button', { name: /Run it in this browser/ }).click();
  await expect(source(page)).toHaveText('this browser', { timeout: 30_000 });

  const stop = page.getByRole('button', { name: /Stop the browser run/ });
  await expect(stop).toBeVisible();
  await stop.click();

  await expect(source(page)).toHaveText('published run');
  await expect(
    page.getByRole('button', { name: /Run it in this browser/ }),
  ).toBeVisible();
});

test('says what the browser run gives up before it is started', async ({ page }) => {
  await page.goto('./?run=collision&renderer=webgl2');

  const start = page.getByRole('button', { name: /Run it in this browser/ });
  const describes = await start.getAttribute('aria-describedby');
  expect(describes).not.toBeNull();

  // The note is a real element rather than a title attribute, as every other
  // explanation in the console is, so it is there for a reader who cannot hover.
  const note = page.locator(`#${describes}`);
  await expect(note).toBeVisible();
  await expect(note).toContainText('WebAssembly');

  // Once the module has said what it will accept, the note says what the run
  // was cut to. The collision publishes more particles than the module runs.
  await start.click();
  await expect(note).toContainText(/cut to [\d ]+ bodies/, { timeout: 30_000 });
});
