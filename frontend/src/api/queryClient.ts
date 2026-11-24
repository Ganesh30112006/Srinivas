import { MutationCache, QueryCache, QueryClient } from '@tanstack/react-query';
import i18n from 'i18next';
import { toast } from '@/components/Toast';
import { describeError } from './describeError';
import { isApiError } from './errors';
import { queryKeys } from './queryKeys';

declare module '@tanstack/react-query' {
  interface Register {
    mutationMeta: {
      /**
       * Set on actions whose failure is not shown in the page itself (delete, publish, …): a
       * failure then shows a toast with this title ("Couldn't delete the lecture.") and the reason.
       * Forms that show their own error banner leave it unset.
       */
      errorTitle?: string;
    };
  }
}

const MAX_RETRIES = 2;

export function createQueryClient(): QueryClient {
  // Any 401 means the session is gone: clear `me`, and the protected shell redirects to /login,
  // which says why. (A 401 from a wrong password on the sign-in page is not a session ending.)
  const onUnauthorized = (error: unknown): boolean => {
    if (!isApiError(error) || !error.isUnauthorized) return false;
    if (client.getQueryData(queryKeys.me())) {
      client.setQueryData(queryKeys.sessionEnded(), true);
      client.setQueryData(queryKeys.me(), null);
    }
    return true;
  };

  const client: QueryClient = new QueryClient({
    queryCache: new QueryCache({
      onError: (error) => {
        onUnauthorized(error);
      },
    }),
    mutationCache: new MutationCache({
      onError: (error, _variables, _context, mutation) => {
        if (onUnauthorized(error)) return; // the sign-in page explains it
        const title = mutation.meta?.errorTitle;
        if (title !== undefined) toast.error(title, describeError(error, i18n.t).message);
      },
    }),
    defaultOptions: {
      queries: {
        staleTime: 30_000,
        gcTime: 10 * 60_000,
        refetchOnWindowFocus: true,
        retry: (failureCount, error) =>
          failureCount < MAX_RETRIES && (!isApiError(error) || error.isRetryable),
        retryDelay: (attempt) => Math.min(1000 * 2 ** attempt, 8000),
      },
      mutations: {
        retry: false,
      },
    },
  });
  return client;
}

/** App-wide instance (tests create their own with createQueryClient). */
export const queryClient = createQueryClient();
