import { expect, test } from '@playwright/test';

/**
 * The console, and what it is honest about.
 *
 * The interesting property is not that the live controls work. It is that the
 * ones which cannot work say so, stay reachable, and carry the reason with
 * them, because the alternative is a control that appears to do something and
 * does not.
 */

test('marks every control that cannot act, and says what it would need', async ({
  page,
}) => {
  await page.goto('./?renderer=webgl2');

  const cannot = ['Trails', 'Lab frame', 'Bulge only', 'Bound / unbound'];
  for (const name of cannot) {
    const control = page.getByRole('button', { name });
    await expect(control).toHaveAttribute('aria-disabled', 'true');

    // The reason is a real element on the page rather than a title attribute,
    // so it is there for a reader who cannot hover.
    const describes = await control.getAttribute('aria-describedby');
    expect(describes).not.toBeNull();
    await expect(page.locator(`#${describes}`)).toBeVisible();
  }

  const recompute = page.getByRole('button', { name: /Recompute/ });
  await expect(recompute).toHaveAttribute('aria-disabled', 'true');
});

test('says the gallery is unaffected when it cannot reach the compute service', async ({
  page,
}) => {
  // The published site is built without a compute service, so this is the state
  // most readers see, and it is the one that has to stay useful: the solver
  // tier explains that a new run needs something this page cannot reach, and
  // says in the same breath that everything else on screen still comes from a
  // file the repository produced.
  await page.goto('./?renderer=webgl2');

  const recompute = page.getByRole('button', { name: /Recompute/ });
  const describes = await recompute.getAttribute('aria-describedby');
  expect(describes).not.toBeNull();

  const note = page.locator(`#${describes}`);
  await expect(note).toBeVisible();
  await expect(note).toContainText('compute service');
  await expect(note).toContainText('unaffected');

  // And the gallery is still there to be used.
  await expect(page.getByRole('link', { name: /Plummer sphere/ })).toBeVisible();
});

test('leaves a control that cannot act in the tab order', async ({ page }) => {
  await page.goto('./?renderer=webgl2');

  // A control removed from the tab order takes its explanation with it, which
  // is exactly the reader who most needs to be told why nothing happened.
  const trails = page.getByRole('button', { name: 'Trails' });
  await trails.focus();
  await expect(trails).toBeFocused();
});

test('changes the picture when the tone curve is changed', async ({ page }) => {
  await page.goto('./?renderer=webgl2');
  const plate = page.locator('#plate canvas');
  await expect(plate).toBeVisible();

  // Long enough for frames to have arrived and for something to be on the
  // plate; the test compares two pictures of the same frame.
  await expect
    .poll(
      async () => {
        const label = await plate.getAttribute('aria-label');
        return Number(/(\d+) of \d+ frames read/.exec(label ?? '')?.[1] ?? 0);
      },
      { timeout: 20_000 },
    )
    .toBeGreaterThan(20);

  const reinhard = await plate.screenshot();
  await page.getByText('linear', { exact: true }).click();
  await page.waitForTimeout(300);
  const linear = await plate.screenshot();

  // The extended Reinhard curve compresses the bright end, so removing it
  // changes the picture. What is asserted is that it changed, not how: how it
  // changes is a matter for the eye and for docs/instrument.md.
  expect(Buffer.compare(reinhard, linear)).not.toBe(0);
});
