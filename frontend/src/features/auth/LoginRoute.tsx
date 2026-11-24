import { useQueryClient } from '@tanstack/react-query';
import { Clock, Mic } from 'lucide-react';
import { useNavigate, useSearchParams } from 'react-router';
import { useTranslation } from 'react-i18next';
import type { TFunction } from 'i18next';
import { describeError } from '@/api/describeError';
import { isApiError } from '@/api/errors';
import { queryKeys } from '@/api/queryKeys';
import { Banner } from '@/components/Feedback';
import { homeFor } from '@/app/guards';
import { useDocumentTitle } from '@/app/useDocumentTitle';
import { safeRedirectPath } from '@/lib/safeRedirect';
import { useLogin } from './api';
import { LoginForm, type LoginValues } from './LoginForm';
import styles from './Login.module.css';

export function loginErrorMessage(error: unknown, t: TFunction<'auth'>, tc: TFunction): string {
  if (isApiError(error) && error.isRateLimited) return t('errors.rateLimited');
  if (isApiError(error) && error.code === 'account_disabled') return t('errors.accountDisabled');
  if (isApiError(error) && error.isUnauthorized) return t('errors.invalidCredentials');
  // Server down, offline, a proxy error…: say which, rather than just "failed".
  return `${t('errors.generic')} ${describeError(error, tc).message}`;
}

export function LoginRoute() {
  const { t } = useTranslation('auth');
  const { t: tc } = useTranslation();
  useDocumentTitle(t('title'));
  const navigate = useNavigate();
  const [params] = useSearchParams();
  const login = useLogin();
  const queryClient = useQueryClient();
  const sessionEnded = queryClient.getQueryData<boolean>(queryKeys.sessionEnded()) === true;

  const submit = (values: LoginValues): void => {
    login.mutate(values, {
      onSuccess: (user) => {
        void navigate(safeRedirectPath(params.get('next'), homeFor(user)), { replace: true });
      },
    });
  };

  return (
    <main id="main-content" className={styles.page}>
      <div className={styles.card}>
        <p className={styles.brand}>
          <Mic aria-hidden="true" />
          {tc('appName')}
        </p>
        <h1 className={styles.title}>{t('title')}</h1>
        <p className={styles.subtitle}>{t('subtitle')}</p>
        {sessionEnded && !login.isError && (
          <Banner tone="info" icon={Clock} className={styles.notice}>
            {t('sessionEnded')}
          </Banner>
        )}
        <LoginForm
          onSubmit={submit}
          pending={login.isPending}
          errorMessage={login.isError ? loginErrorMessage(login.error, t, tc) : null}
        />
      </div>
    </main>
  );
}
