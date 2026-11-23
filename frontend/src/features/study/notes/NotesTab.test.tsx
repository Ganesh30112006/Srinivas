import { screen } from '@testing-library/react';
import { http, HttpResponse } from 'msw';
import { describe, expect, it } from 'vitest';
import { env } from '@/lib/env';
import { LECTURE_IDS } from '@/test/fixtures/lectures';
import { server } from '@/test/msw/node';
import { renderApp } from '@/test/renderApp';

// The first render in a file pays the app's cold import cost, which is slow when every test file
// runs in parallel; give it room (same convention as TranscriptTab.test.tsx).
const SLOW = { timeout: 20_000 };
const FIRST_RENDER = { timeout: 10_000 };

/** The API's answer when generation failed (an AI outage) and nobody has regenerated yet. */
function notesFailed(): void {
  server.use(
    http.get(`${env.apiBaseUrl}/lectures/:id/notes`, () =>
      HttpResponse.json(
        { code: 'notes_failed', message: 'Generation failed. A lecturer can regenerate it.' },
        { status: 404 },
      ),
    ),
  );
}

describe('NotesTab', () => {
  it('tells a student that notes failed instead of "still being generated"', SLOW, async () => {
    notesFailed();
    renderApp(`/lectures/${LECTURE_IDS.scheduling}/notes`, { as: 'arjun' });
    expect(
      await screen.findByText("This couldn't be generated", undefined, FIRST_RENDER),
    ).toBeInTheDocument();
    expect(screen.queryByText('Notes are still being generated')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Regenerate' })).not.toBeInTheDocument();
  });

  it('offers the lecturer a way to regenerate failed notes', SLOW, async () => {
    notesFailed();
    let regenerated = false;
    server.use(
      http.post(`${env.apiBaseUrl}/lectures/:id/notes/regenerate`, () => {
        regenerated = true;
        return HttpResponse.json({}, { status: 202 });
      }),
    );
    const { user } = renderApp(`/lectures/${LECTURE_IDS.scheduling}/notes`, { as: 'priya' });
    await user.click(await screen.findByRole('button', { name: 'Regenerate' }, FIRST_RENDER));
    expect(regenerated).toBe(true);
  });
});
