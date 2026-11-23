import { spawn, type ChildProcess } from 'node:child_process';
import { expect, test, type Page } from '@playwright/test';

/**
 * Full-stack E2E: the real API, gateway, ASR worker (faster-whisper) and notes worker, with the
 * device simulator streaming a spoken WAV. See e2e-stack/README.md for how to start the stack.
 */
function env(name: string): string {
  const value = process.env[name];
  if (value === undefined || value === '') throw new Error(`Set ${name} (see e2e-stack/README.md)`);
  return value;
}

const PASSWORD = env('STACK_PASSWORD');
const ACCOUNTS = {
  admin: 'admin@college.example',
  lecturer: 'lecturer@college.example',
  student: 'student@college.example',
} as const;
type Role = keyof typeof ACCOUNTS;

/**
 * Expected error responses: the signed-out session probe on the login page, and 404 from study
 * material that is not ready / not published / failed (each shown as its own empty state).
 */
const STUDY = /\/api\/lectures\/[^/]+\/(notes|summary|key-terms|questions|flashcards|revision)$/;
const EXPECTED = (status: number, url: string): boolean =>
  (status === 401 && url.endsWith('/api/auth/me')) || (status === 404 && STUDY.test(url));

/** Collects uncaught page errors, console errors and unexpected 4xx/5xx responses. */
function watchForProblems(page: Page): string[] {
  const problems: string[] = [];
  page.on('pageerror', (error) => problems.push(`pageerror: ${error.message}`));
  page.on('console', (message) => {
    // Failed requests are judged by URL in the response handler below.
    if (message.type() === 'error' && !message.text().startsWith('Failed to load resource'))
      problems.push(`console: ${message.text()}`);
  });
  page.on('response', (response) => {
    const status = response.status();
    if (status >= 400 && !EXPECTED(status, response.url()))
      problems.push(`${String(status)} ${response.url()}`);
  });
  return problems;
}

/** A 1-6 word phrase said three or more times in a row (a Whisper decoding loop), or null. */
function repeatedPhrase(text: string): string | null {
  const words = text.toLowerCase().match(/[\p{L}\p{N}']+/gu) ?? [];
  for (let i = 0; i < words.length; i += 1) {
    for (let n = 1; n <= 6; n += 1) {
      const phrase = words.slice(i, i + n).join(' ');
      const second = words.slice(i + n, i + 2 * n).join(' ');
      const third = words.slice(i + 2 * n, i + 3 * n).join(' ');
      if (phrase !== '' && phrase === second && phrase === third) return phrase;
    }
  }
  return null;
}

async function signIn(page: Page, role: Role, next = '/'): Promise<void> {
  await page.goto(`/login?next=${encodeURIComponent(next)}`);
  await page.getByLabel('Email').fill(ACCOUNTS[role]);
  await page.getByLabel('Password', { exact: true }).fill(PASSWORD);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await expect(page).not.toHaveURL(/\/login/);
}

function startSimulator(lectureId: string): { done: Promise<number>; child: ChildProcess } {
  const child = spawn(
    env('STACK_PYTHON'),
    [
      '-m',
      'tools.device_simulator',
      '--url',
      'ws://127.0.0.1:8001/ws/device',
      '--device-id',
      env('STACK_DEVICE_ID'),
      '--token',
      env('STACK_DEVICE_TOKEN'),
      '--wav',
      env('STACK_WAV'),
      '--lecture-id',
      lectureId,
      '--outage',
      '30:10',
    ],
    { cwd: env('STACK_BACKEND_DIR'), stdio: 'inherit' },
  );
  const done = new Promise<number>((resolve) => {
    child.on('exit', (code) => {
      resolve(code ?? -1);
    });
  });
  return { done, child };
}

const TITLE = `CPU scheduling ${String(Date.now())}`;
let lectureId = '';

test.describe.configure({ mode: 'serial' });

test('a lecturer records a lecture and watches the real transcript arrive live', async ({
  page,
}) => {
  test.setTimeout(240_000);
  const problems = watchForProblems(page);
  await signIn(page, 'lecturer');

  await page.getByLabel('Subject').selectOption({ label: 'CS501 · Operating Systems' });
  await page.getByLabel('Lecture title').fill(TITLE);
  await page.getByLabel('Microphone', { exact: true }).selectOption({ index: 1 });
  await page.getByText('Students know this lecture is recorded').click();
  await page.getByRole('button', { name: 'Prepare lecture' }).click();
  await expect(page).toHaveURL(/\/lectures\/[^/]+\/live$/);
  lectureId = /\/lectures\/([^/]+)\/live$/.exec(page.url())?.[1] ?? '';
  await expect(page.getByRole('heading', { name: TITLE })).toBeVisible();

  const sim = startSimulator(lectureId);
  try {
    const transcript = page.getByRole('list', { name: 'Live transcript' });
    // Real speech recognition of the synthetic lecture: the opening sentence comes through live.
    await expect(transcript).toContainText(/scheduling/i, { timeout: 90_000 });
    // The 10 s outage at 0:30 is recorded offline and backfilled; the whole lecture arrives.
    // Whisper spells it either way from run to run.
    await expect(transcript).toContainText(/multi-?level feedback/i, { timeout: 150_000 });
    expect(await sim.done).toBe(0);
  } finally {
    sim.child.kill();
  }
  await page.screenshot({ path: 'test-results/stack-live.png' });
  expect(problems).toEqual([]);
});

test('the finished lecture has a searchable transcript and a clear notes state', async ({
  page,
}) => {
  test.setTimeout(180_000);
  expect(lectureId).not.toBe('');
  const problems = watchForProblems(page);
  await signIn(page, 'student', `/lectures/${lectureId}/transcript`);

  // Processing finishes (Bedrock is deliberately unreachable here, so notes fail cleanly).
  await expect
    .poll(
      async () => {
        await page.reload();
        // isVisible() does not wait, so let the lecture load before reading its status.
        await expect(page.getByRole('heading', { name: TITLE })).toBeVisible();
        return page.getByText('Completed', { exact: true }).first().isVisible();
      },
      { timeout: 150_000, intervals: [3_000] },
    )
    .toBe(true);

  const rows = page.locator('[data-segment-id]');
  await expect(rows.first()).toBeVisible();
  // The spoken text only (not the language tag or a transliteration line).
  const lines = await rows.locator('p[lang]:not([lang$="-Latn"])').allTextContents();
  const text = lines.join(' ');
  expect(text).toMatch(/convoy effect/i);
  expect(text).toMatch(/round robin/i);
  // Content that was once lost to a decoding loop or a truncated window.
  expect(text).toMatch(/waiting time/i);
  expect(text).toMatch(/response time/i);
  expect(text).toMatch(/time quantum/i);
  // Sentence openings right after a pause are kept (they were once lost at the window cut).
  expect(text).toMatch(/\bwe measure\b/i);
  expect(text).toMatch(/\bthe simplest\b/i);
  // No word repeated across a window cut ("for For example"), no invented "you" on silence.
  expect(text).not.toMatch(/\b(\w+)[,.]? \1\b/i);
  expect(lines.map((line) => line.trim().toLowerCase())).not.toContain('you');
  // No decoding loop ("the simplest algorithm is the simplest algorithm is ...").
  expect(repeatedPhrase(text)).toBeNull();
  // No sampled junk from other scripts (seen once: "throughput, U+90B8 rup ..."): CJK, Hangul.
  expect(text).not.toMatch(/[\u2E80-\u9FFF\uAC00-\uD7AF]/u);
  // Every word exactly once: the backfilled outage must not duplicate lines.
  const ids = await rows.evaluateAll((els) => els.map((el) => el.getAttribute('data-segment-id')));
  expect(new Set(ids).size).toBe(ids.length);
  await page.screenshot({ path: 'test-results/stack-transcript.png', fullPage: true });

  await page.getByRole('tab', { name: 'Notes', exact: true }).click();
  // Bedrock is unreachable in this stack: say so, rather than "still being generated".
  await expect(page.getByText("This couldn't be generated")).toBeVisible();

  await page.goto('/search?q=convoy');
  await expect(page.getByRole('link', { name: TITLE })).toBeVisible();
  await expect(page.getByText(/1 match|\d+ matches/)).toBeVisible();

  await page.goto('/lectures');
  await expect(page.getByText(TITLE)).toBeVisible();
  expect(problems).toEqual([]);
});

test('admin and settings pages work against the real API', async ({ page }) => {
  const problems = watchForProblems(page);
  await signIn(page, 'admin', '/devices');
  await expect(page.getByText('Desk mic · Room 204')).toBeVisible();
  await page.goto('/admin/users');
  for (const email of Object.values(ACCOUNTS)) await expect(page.getByText(email)).toBeVisible();
  await page.goto('/admin/subjects');
  await expect(page.getByText('Theory of Computation')).toBeVisible();
  await page.goto('/settings');
  await expect(page.getByRole('heading', { name: 'Settings' })).toBeVisible();
  expect(problems).toEqual([]);
});
