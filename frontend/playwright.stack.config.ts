import { defineConfig, devices } from '@playwright/test';

const channel = process.env.PW_CHANNEL;

/**
 * Full-stack E2E (`npm run e2e:stack`): the Vite dev server proxies to a real running backend.
 * Nothing is started here; see e2e-stack/README.md.
 */
export default defineConfig({
  testDir: 'e2e-stack',
  reporter: [['list']],
  workers: 1,
  timeout: 120_000,
  expect: { timeout: 15_000 },
  use: {
    ...devices['Desktop Chrome'],
    ...(channel === undefined ? {} : { channel }),
    baseURL: process.env.STACK_URL ?? 'http://localhost:5173',
    trace: 'retain-on-failure',
    locale: 'en-IN',
    timezoneId: 'Asia/Kolkata',
  },
});
