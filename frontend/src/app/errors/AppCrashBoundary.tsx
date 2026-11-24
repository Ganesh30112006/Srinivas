import { Component, type ErrorInfo, type ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { ErrorState } from '@/components/Feedback';
import { logger } from '@/lib/logger';
import styles from './AppCrash.module.css';

function AppCrash({ error }: { error: unknown }) {
  const { t } = useTranslation('shell');
  return (
    <main id="main-content" className={styles.page}>
      <ErrorState
        title={t('appCrash.title')}
        message={t('appCrash.body')}
        error={error}
        onRetry={() => {
          window.location.reload();
        }}
      />
    </main>
  );
}

/**
 * Last line of defence, around the router itself. Each route has its own error element, so this
 * only shows when something outside them fails; without it the screen would go blank.
 */
export class AppCrashBoundary extends Component<{ children: ReactNode }, { error: unknown }> {
  override state: { error: unknown } = { error: null };

  static getDerivedStateFromError(error: unknown): { error: unknown } {
    return { error: error ?? new Error('Unknown error') };
  }

  override componentDidCatch(error: Error, info: ErrorInfo): void {
    logger.error('app crashed', { error, componentStack: info.componentStack });
  }

  override render(): ReactNode {
    return this.state.error === null ? this.props.children : <AppCrash error={this.state.error} />;
  }
}
