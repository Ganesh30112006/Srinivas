import { render, screen, waitFor, within } from '@testing-library/react';
import { http, HttpResponse } from 'msw';
import { describe, expect, it, vi } from 'vitest';
import { env } from '@/lib/env';
import { LECTURE_IDS } from '@/test/fixtures/lectures';
import { server } from '@/test/msw/node';
import { renderApp } from '@/test/renderApp';
import { AppCrashBoundary } from './AppCrashBoundary';

const SLOW = { timeout: 20_000 };
const FIRST_RENDER = { timeout: 10_000 };
const lecture = `${env.apiBaseUrl}/lectures/:id`;

describe('error pages and messages', () => {
  it('says why a page failed, with a code, and offers Retry', SLOW, async () => {
    let calls = 0;
    server.use(
      http.get(lecture, () => {
        calls += 1;
        return HttpResponse.json(
          { code: 'service_unavailable', message: "The server can't reach its database." },
          { status: 503 },
        );
      }),
    );
    const { user } = renderApp(`/lectures/${LECTURE_IDS.scheduling}/transcript`);
    const alert = await screen.findByRole('alert', undefined, FIRST_RENDER);
    expect(within(alert).getByRole('heading')).toHaveTextContent("Couldn't load this lecture");
    expect(alert).toHaveTextContent("The server can't reach its database.");
    expect(alert).toHaveTextContent('Error code: service_unavailable · 503');
    const before = calls;
    await user.click(within(alert).getByRole('button', { name: 'Try again' }));
    await waitFor(() => {
      expect(calls).toBeGreaterThan(before);
    });
  });

  it('explains a server that cannot be reached at all', SLOW, async () => {
    server.use(http.get(lecture, () => HttpResponse.error()));
    renderApp(`/lectures/${LECTURE_IDS.scheduling}/transcript`);
    expect(
      await screen.findByText(/^Couldn't reach the server/, undefined, FIRST_RENDER),
    ).toBeInTheDocument();
  });

  it('shows a toast with the reason when an action fails', SLOW, async () => {
    server.use(
      http.delete(lecture, () =>
        HttpResponse.json(
          {
            code: 'lecture_recording',
            message: 'Stop the recording on the device before deleting.',
          },
          { status: 409 },
        ),
      ),
    );
    const { user } = renderApp(`/lectures/${LECTURE_IDS.scheduling}/transcript`);
    await user.click(await screen.findByRole('button', { name: 'Delete lecture' }, FIRST_RENDER));
    await user.click(
      within(screen.getByRole('dialog')).getByRole('button', { name: 'Delete lecture' }),
    );
    expect(await screen.findByText("Couldn't delete the lecture.")).toBeInTheDocument();
    expect(
      screen.getByText('Stop the recording on the device before deleting.'),
    ).toBeInTheDocument();
  });

  it('tells the user when their session has ended', SLOW, async () => {
    const { router } = renderApp('/');
    await screen.findByRole('heading', { level: 1 }, FIRST_RENDER);
    server.use(
      http.get(`${env.apiBaseUrl}/lectures`, () =>
        HttpResponse.json(
          { code: 'not_authenticated', message: 'Sign in to continue.' },
          { status: 401 },
        ),
      ),
    );
    await router.navigate('/lectures');
    expect(
      await screen.findByText('Your session has ended. Sign in again to continue where you were.'),
    ).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/login');
  });

  it('does not call a wrong password an ended session', SLOW, async () => {
    const { user } = renderApp('/login', { as: null });
    await user.type(
      await screen.findByLabelText('Email', undefined, FIRST_RENDER),
      'arjun.kumar@college.example',
    );
    await user.type(screen.getByLabelText('Password'), 'not-the-password');
    await user.click(screen.getByRole('button', { name: 'Sign in' }));
    expect(await screen.findByText('Email or password is incorrect.')).toBeInTheDocument();
    expect(screen.queryByText(/session has ended/)).not.toBeInTheDocument();
  });

  it('explains a page that does not exist', SLOW, async () => {
    renderApp('/no/such/page');
    expect(
      await screen.findByRole('heading', { name: 'Page not found' }, FIRST_RENDER),
    ).toBeInTheDocument();
    expect(screen.getByText(/The link may be wrong/)).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Go to home' })).toBeInTheDocument();
  });

  it('replaces a blank screen with a reload page when the app itself crashes', () => {
    vi.spyOn(console, 'error').mockImplementation(() => undefined);
    function Broken(): never {
      throw new Error('boom');
    }
    render(
      <AppCrashBoundary>
        <Broken />
      </AppCrashBoundary>,
    );
    expect(screen.getByRole('heading', { name: 'Something went wrong' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
    vi.restoreAllMocks();
  });
});
