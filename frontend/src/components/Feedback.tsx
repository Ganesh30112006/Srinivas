import {
  CircleAlert,
  CircleCheck,
  Info,
  RefreshCw,
  TriangleAlert,
  type LucideIcon,
} from 'lucide-react';
import type { ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { canRetry, describeError } from '@/api/describeError';
import { cx } from '@/lib/cx';
import { Button } from './Button';
import styles from './Feedback.module.css';

type Tone = 'info' | 'warning' | 'danger' | 'success';

const TONE_ICON: Record<Tone, LucideIcon> = {
  info: Info,
  warning: TriangleAlert,
  danger: CircleAlert,
  success: CircleCheck,
};

interface BannerProps {
  tone: Tone;
  children: ReactNode;
  action?: ReactNode;
  icon?: LucideIcon;
  /** "alert" interrupts screen readers: only for urgent, user-blocking problems. */
  role?: 'status' | 'alert' | 'none';
  className?: string | undefined;
}

export function Banner({ tone, children, action, icon, role = 'status', className }: BannerProps) {
  const Icon = icon ?? TONE_ICON[tone];
  return (
    <div
      className={cx(styles.banner, styles[tone], className)}
      role={role === 'none' ? undefined : role}
    >
      <Icon aria-hidden="true" />
      <div className={styles.bannerBody}>
        <div className={styles.bannerText}>{children}</div>
        {action}
      </div>
    </div>
  );
}

/** Decorative placeholder; wrap groups in an element with aria-busy and a visually hidden label. */
export function Skeleton({ className }: { className?: string | undefined }) {
  return <span className={cx(styles.skeleton, className)} aria-hidden="true" />;
}

export function LoadingRegion({
  label,
  children,
  className,
}: {
  label: string;
  children: ReactNode;
  className?: string | undefined;
}) {
  return (
    <div className={className} aria-busy="true">
      <span className="visually-hidden" role="status">
        {label}
      </span>
      {children}
    </div>
  );
}

interface EmptyStateProps {
  icon: LucideIcon;
  title: string;
  body?: ReactNode;
  action?: ReactNode;
  compact?: boolean;
  headingLevel?: 2 | 3;
}

export function EmptyState({
  icon: Icon,
  title,
  body,
  action,
  compact = false,
  headingLevel = 2,
}: EmptyStateProps) {
  const Heading = headingLevel === 2 ? 'h2' : 'h3';
  return (
    <div className={cx(styles.state, compact && styles.compact)}>
      <Icon aria-hidden="true" />
      <Heading className={styles.stateTitle}>{title}</Heading>
      {body !== undefined && <p className={styles.stateBody}>{body}</p>}
      {action}
    </div>
  );
}

interface ErrorStateProps {
  title: string;
  /** Overrides the explanation derived from `error`. */
  message?: string;
  /** The failure: explained in plain words, with a short code for support. */
  error?: unknown;
  onRetry?: () => void;
  retrying?: boolean;
  compact?: boolean;
  headingLevel?: 2 | 3;
  /** Another way forward, e.g. a link home. */
  action?: ReactNode;
}

/**
 * What failed (title), why and what to do (message), and Retry when retrying can help.
 * Rendered with role="alert" so the failure is announced.
 */
export function ErrorState({
  title,
  message,
  error,
  onRetry,
  retrying = false,
  compact = false,
  headingLevel = 2,
  action,
}: ErrorStateProps) {
  const { t } = useTranslation();
  const Heading = headingLevel === 2 ? 'h2' : 'h3';
  const described = error === undefined ? null : describeError(error, t);
  const body = message ?? described?.message;
  const showRetry = onRetry !== undefined && (error === undefined || canRetry(error));
  return (
    <div className={cx(styles.state, compact && styles.compact)} role="alert">
      <CircleAlert aria-hidden="true" className={styles.errorIcon} />
      <Heading className={styles.stateTitle}>{title}</Heading>
      {body !== undefined && <p className={styles.stateBody}>{body}</p>}
      {described?.code != null && (
        <p className={styles.errorCode}>{t('errors.code', { code: described.code })}</p>
      )}
      {(showRetry || action !== undefined) && (
        <div className={styles.stateActions}>
          {showRetry && (
            <Button icon={RefreshCw} onClick={onRetry} loading={retrying}>
              {t('actions.retry')}
            </Button>
          )}
          {action}
        </div>
      )}
    </div>
  );
}

/**
 * A failed action inside a form or dialog: what failed, then why ("Couldn't save the settings.
 * The server is unavailable right now…"). The inputs stay as they were, so the user can retry.
 */
export function ErrorBanner({
  title,
  error,
  className,
}: {
  title: string;
  error: unknown;
  className?: string | undefined;
}) {
  const { t } = useTranslation();
  const { message } = describeError(error, t);
  return (
    <Banner tone="danger" role="alert" className={className}>
      <strong>{title}</strong> {message}
    </Banner>
  );
}
