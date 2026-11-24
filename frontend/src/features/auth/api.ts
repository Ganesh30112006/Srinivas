import { queryOptions, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { api, request } from '@/api/client';
import { isApiError } from '@/api/errors';
import { queryKeys } from '@/api/queryKeys';
import type { User } from '@/api/types';

/** The signed-in user, or null when there is no session (401 is an expected answer here). */
export const meQueryOptions = queryOptions({
  queryKey: queryKeys.me(),
  queryFn: async (): Promise<User | null> => {
    try {
      return await request(api.GET('/auth/me'));
    } catch (error) {
      if (isApiError(error) && error.isUnauthorized) return null;
      throw error;
    }
  },
  staleTime: 5 * 60_000,
});

export function useMe() {
  return useQuery(meQueryOptions);
}

/** Current user inside the protected shell (the route loader guarantees a session). */
export function useCurrentUser(): User | null {
  return useMe().data ?? null;
}

export interface Credentials {
  email: string;
  password: string;
}

export function useLogin() {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (credentials: Credentials) =>
      request(api.POST('/auth/login', { body: credentials })),
    onSuccess: (user) => {
      // A new session starts empty: after an expired session (no sign-out) on a shared lab PC, the
      // previous user's cached pages, e.g. a lecturer's draft notes, must not show for the next one.
      queryClient.clear();
      queryClient.setQueryData(queryKeys.me(), user);
    },
  });
}

export function useLogout() {
  const queryClient = useQueryClient();
  const { t } = useTranslation('auth');
  return useMutation({
    // Signed out on this device anyway, but the server session may still be open: say so.
    meta: { errorTitle: t('signOutFailed') },
    mutationFn: () => request(api.POST('/auth/logout')),
    // Even if the request fails, forget everything locally: the user asked to leave.
    onSettled: () => {
      queryClient.clear();
      queryClient.setQueryData(queryKeys.me(), null);
    },
  });
}
