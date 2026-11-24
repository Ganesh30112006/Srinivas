import i18n from 'i18next';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { canRetry, describeError } from './describeError';
import { ApiError } from './errors';

const t = i18n.t;
const http = (status: number, code: string, message = '') =>
  new ApiError('http', status, code, message);

describe('describeError', () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('says whether the user is offline or the server is unreachable', () => {
    vi.spyOn(navigator, 'onLine', 'get').mockReturnValue(false);
    expect(describeError(ApiError.network(null), t)).toEqual({
      message: "You're offline. Check your internet connection, then try again.",
      code: null,
    });
    vi.spyOn(navigator, 'onLine', 'get').mockReturnValue(true);
    expect(describeError(ApiError.network(null), t).message).toMatch(/^Couldn't reach the server/);
  });

  it("shows the server's own message, with its code for support", () => {
    expect(
      describeError(
        http(409, 'device_busy', 'This device is already recording another lecture.'),
        t,
      ),
    ).toEqual({
      message: 'This device is already recording another lecture.',
      code: 'device_busy · 409',
    });
  });

  it('explains generic HTTP failures (a proxy error has no useful message)', () => {
    expect(describeError(http(502, 'http_502', 'Bad Gateway'), t)).toEqual({
      message: 'The server is unavailable right now, perhaps restarting. Try again in a minute.',
      code: 'HTTP 502',
    });
    expect(describeError(http(500, 'http_500'), t).message).toMatch(/went wrong on the server/);
    expect(describeError(http(403, 'http_403'), t).message).toBe(
      "Your account doesn't have permission to do this.",
    );
    expect(describeError(http(418, 'http_418'), t).message).toBe(
      "The server couldn't complete this request (status 418).",
    );
  });

  it('introduces validation details', () => {
    expect(
      describeError(http(422, 'validation_error', 'Title: should have at least 3 characters'), t)
        .message,
    ).toBe('Check what you entered: Title: should have at least 3 characters');
  });

  it('treats anything else as a problem in the app itself', () => {
    expect(describeError(new TypeError('x is undefined'), t)).toEqual({
      message: 'Something went wrong in the app. Reload the page, then try again.',
      code: 'TypeError',
    });
  });

  it('offers a retry only when it can help', () => {
    expect(canRetry(ApiError.network(null))).toBe(true);
    expect(canRetry(http(503, 'service_unavailable'))).toBe(true);
    expect(canRetry(http(429, 'rate_limited'))).toBe(true);
    expect(canRetry(http(404, 'lecture_not_found'))).toBe(false);
    expect(canRetry(http(403, 'forbidden'))).toBe(false);
  });
});
