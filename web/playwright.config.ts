import { defineConfig, devices } from '@playwright/test';

// The browser tests run against the production build rather than the dev
// server, because the things they check, the fonts arriving, the bundle
// executing, the focus order, are properties of what is published.
export default defineConfig({
  testDir: 'e2e',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? 'github' : 'list',
  use: {
    baseURL: 'http://localhost:4173/orrery/instrument/',
    trace: 'on-first-retry',
  },
  projects: [{ name: 'chromium', use: { ...devices['Desktop Chrome'] } }],
  webServer: {
    command: 'npm run preview -- --port 4173 --strictPort',
    url: 'http://localhost:4173/orrery/instrument/',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
