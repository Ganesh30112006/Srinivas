import type { TFunction } from 'i18next';
import { isApiError } from './errors';

export interface ErrorDescription {
  /** Why it failed and what to do, in one or two plain sentences. */
  message: string;
  /** Short reference for support ("device_busy · 409"), or null when there is nothing useful. */
  code: string | null;
}

function isOffline(): boolean {
  return typeof navigator !== 'undefined' && !navigator.onLine;
}

/** The status-based explanation, used when the server did not send its own message. */
function byStatus(status: number, t: TFunction): string {
  if (status === 401) return t('errors.sessionEnded');
  if (status === 403) return t('errors.forbidden');
  if (status === 404) return t('errors.notFound');
  if (status === 408 || status === 504) return t('errors.timeout');
  if (status === 409) return t('errors.conflict');
  if (status === 413) return t('errors.tooLarge');
  if (status === 429) return t('errors.rateLimited');
  if (status === 502 || status === 503) return t('errors.unavailable');
  if (status >= 500) return t('errors.server');
  return t('errors.request', { status });
}

/**
 * Turns any thrown value into a message a lecturer or student can act on. The backend's own
 * messages are written for users ({code, message}), so they are shown as they are; generic HTTP
 * failures (a proxy's 502, a timeout) and network failures get an explanation by kind.
 */
export function describeError(error: unknown, t: TFunction): ErrorDescription {
  if (!isApiError(error)) {
    return {
      message: t('errors.unexpected'),
      code: error instanceof Error && error.name !== 'Error' ? error.name : null,
    };
  }
  if (error.isNetwork) {
    return { message: isOffline() ? t('errors.offline') : t('errors.network'), code: null };
  }
  const generic = error.code.startsWith('http_');
  const code = generic ? `HTTP ${String(error.status)}` : `${error.code} · ${String(error.status)}`;
  if (!generic && error.message.trim() !== '') {
    const message =
      error.code === 'validation_error'
        ? t('errors.validation', { details: error.message })
        : error.message;
    return { message, code };
  }
  return { message: byStatus(error.status, t), code };
}

/** Whether trying the same request again can help (not for "no permission" or "not found"). */
export function canRetry(error: unknown): boolean {
  if (!isApiError(error)) return true;
  return error.isRetryable || [408, 409, 429].includes(error.status);
}
