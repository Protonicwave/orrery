import { expect, test } from '@playwright/test';

/**
 * A run submitted from the browser, taken by the service, and played back.
 *
 * This is the one test that exercises the whole system at once: the console
 * builds a configuration, the API validates and queues it, a worker runs the
 * native binary over it, the trajectory goes to object storage and comes back
 * through the API in ranges, and the instrument draws it. Every other test in
 * this repository covers one of those and none of them covers the seam.
 *
 * It needs a service, so the build under test has to have been given one:
 *
 *     docker compose -f deploy/compose.yaml up -d --wait
 *     VITE_ORRERY_SERVICE=http://localhost:8000 npm run build
 *     VITE_ORRERY_SERVICE=http://localhost:8000 npx playwright test e2e/service.spec.ts
 *
 * Without that it skips itself, which is the state the published site is built
 * in and the state the rest of this suite tests. The variable is read here to
 * decide whether to skip; what the page actually talks to is whatever was
 * compiled into the build, so a mismatch shows up as a failure rather than as a
 * test that quietly checked nothing.
 */

const SERVICE = process.env.VITE_ORRERY_SERVICE ?? '';

test.describe('a run taken by the compute service', () => {
  test.skip(
    SERVICE === '',
    'VITE_ORRERY_SERVICE is not set, so this build has no service to submit to',
  );

  // A run of a few thousand particles takes a minute or two on whatever
  // hardware the service has, and it is queued behind whatever else is there.
  test.setTimeout(300_000);

  test('submits it, follows it, and draws what comes back', async ({ page }) => {
    // The two-body problem, because it is the cheapest run the gallery has and
    // this test is about the path rather than about the physics: two bodies
    // under the direct solver is a few seconds of work, where the collision at
    // eight thousand is several minutes of it. What is being checked is that a
    // configuration goes out and a trajectory comes back.
    await page.goto('./?run=kepler&renderer=webgl2');

    // The console asks the service what it will take before it offers to submit
    // anything, so the button is drawn back until that answer has arrived.
    const recompute = page.getByRole('button', { name: /Recompute/ });
    await expect(recompute).not.toHaveAttribute('aria-disabled', 'true', {
      timeout: 30_000,
    });

    // The estimate says which measurement it rests on, and once the service has
    // run anything it is the service's own.
    await expect(recompute).toContainText('est.');

    await recompute.click();

    // Queued, then running, then complete. What is asserted is that it reaches
    // the end; the states in between are reported as the service reports them
    // and a fast service may pass through them faster than this can look.
    const solver = page.getByRole('region', { name: 'Solver' });
    await expect(solver).toContainText(/queued|running|complete/, {
      timeout: 30_000,
    });
    await expect(solver).toContainText('complete', { timeout: 240_000 });

    // The plate's catalogue says where the picture came from, which is the
    // thing that must never be got wrong: a run taken on the service's hardware
    // is not the run in docs/performance.md and not one this browser computed.
    // Located inside the plate rather than by its text, because the same words
    // appear in the notes explaining the tier that submitted it.
    const source = page.locator('#plate dt', { hasText: /^SRC$/ });
    await expect(source.locator('xpath=following-sibling::dd[1]')).toHaveText(
      'the compute service',
    );

    // And it is actually drawing it. The canvas describes the run it holds, and
    // the frame count in that description comes from the trajectory the service
    // produced rather than from the published one.
    const plate = page.locator('#plate canvas');
    await expect
      .poll(
        async () => {
          const label = await plate.getAttribute('aria-label');
          return Number(/(\d+) of \d+ frames read/.exec(label ?? '')?.[1] ?? 0);
        },
        { timeout: 60_000 },
      )
      .toBeGreaterThan(10);
  });

  test('refuses a run it will not take, and says which setting', async ({ page }) => {
    // The ceilings are the service's, and the console reads them rather than
    // holding a copy, so the slider cannot ask for more than the service takes.
    // What this checks is the other half: that the service's refusal, if one
    // ever arrives, is shown as the service worded it.
    await page.goto('./?renderer=webgl2');

    const answer = await page.evaluate(async (service: string) => {
      const response = await fetch(`${service}/jobs`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          configuration: '[run]\ntimestep = 0\nsteps = 0\n',
        }),
      });
      return { status: response.status, body: await response.json() };
    }, SERVICE);

    expect(answer.status).toBe(422);
    const named = (answer.body as { problems: { setting: string }[] }).problems.map(
      (problem) => problem.setting,
    );
    expect(named).toContain('run.timestep');
    expect(named).toContain('run.steps');
  });
});
