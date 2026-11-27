import { useQueryClient } from '@tanstack/react-query';
import { CircleCheck, Mic } from 'lucide-react';
import { useEffect } from 'react';
import { useParams } from 'react-router';
import { useTranslation } from 'react-i18next';
import { isApiError } from '@/api/errors';
import { queryKeys } from '@/api/queryKeys';
import { elapsedMs } from '@/api/ws/liveSession';
import { paths } from '@/app/paths';
import { useDocumentTitle } from '@/app/useDocumentTitle';
import { WidgetErrorBoundary } from '@/app/errors/WidgetErrorBoundary';
import { ButtonLink } from '@/components/Button';
import { EmptyState, ErrorState, LoadingRegion, Skeleton } from '@/components/Feedback';
import { PageHeader } from '@/components/Page';
import { useCurrentUser } from '@/features/auth/api';
import { useLecture } from '@/features/lectures/api';
import { canEditLecture } from '@/features/lectures/permissions';
import { usePrefs } from '@/features/settings/prefsStore';
import { useNow } from '@/lib/useNow';
import { DeviceMonitor } from './DeviceMonitor';
import { ConnectionBanner, DeviceAlertBanner } from './LiveBanners';
import { LiveControls } from './LiveControls';
import { LiveHeader } from './LiveHeader';
import { LiveKeyTermsPanel } from './LiveKeyTermsPanel';
import { LiveTranscriptPanel } from './LiveTranscriptPanel';
import { useLive, useLiveManager, useLiveSession } from './live';
import styles from './Live.module.css';
import { useLiveToasts } from './useLiveToasts';

function LiveScreen({ lectureId }: { lectureId: string }) {
  const { t } = useTranslation('live');
  const { t: tc } = useTranslation();
  const user = useCurrentUser();
  const lecture = useLecture(lectureId);
  const queryClient = useQueryClient();
  const manager = useLiveManager();
  useLiveSession(lectureId);
  const connection = useLive((s) => s.connection);
  const latency = useLive((s) => s.latencyMs);
  const session = useLive((s) => s.session);
  const now = useNow();
  const announce = usePrefs((s) => s.announce);
  const setPref = usePrefs((s) => s.set);
  useDocumentTitle(lecture.data ? t('pageTitle', { title: lecture.data.title }) : null);

  // Server data that the live stream just changed: refresh it (side effect, not derived state).
  useEffect(() => {
    if (session.lectureStatus === 'completed' || session.notesReady.length > 0) {
      void queryClient.invalidateQueries({ queryKey: queryKeys.lectures.detail(lectureId) });
    }
  }, [session.lectureStatus, session.notesReady.length, lectureId, queryClient]);

  useLiveToasts(connection.status, session.lectureStatus, session.notesReady.length);

  if (lecture.isPending) {
    return (
      <LoadingRegion label={tc('loading')} className={styles.loading}>
        <Skeleton className={styles.skeletonTitle} />
        <Skeleton className={styles.skeletonBar} />
        {[0, 1, 2, 3].map((i) => (
          <Skeleton key={i} className={styles.skeletonLine} />
        ))}
      </LoadingRegion>
    );
  }
  if (lecture.isError) {
    const missing = isApiError(lecture.error) && lecture.error.isNotFound;
    return (
      <ErrorState
        title={missing ? t('connectionBanner.notFound') : t('loadError')}
        error={lecture.error}
        onRetry={() => void lecture.refetch()}
      />
    );
  }

  const status = session.lectureStatus ?? lecture.data.status;
  const paused = session.lectureStatus === null ? lecture.data.paused : session.paused;
  const stopped = session.lectureStatus === null ? lecture.data.stopped : session.stopped;
  const isLecturer = user !== null && user.role !== 'student';
  const canControl = canEditLecture(user, lecture.data);
  const ended = status === 'completed' || status === 'processing' || status === 'failed';

  return (
    <>
      <PageHeader
        title={lecture.data.title}
        eyebrow={
          lecture.data.subject
            ? `${lecture.data.subject.code} · ${lecture.data.subject.name}`
            : undefined
        }
      />
      <div className={styles.sticky}>
        <LiveHeader
          status={status}
          paused={paused}
          elapsedMs={elapsedMs(session, now)}
          language={session.latestLanguage}
          connection={connection}
          latencyMs={latency}
          announce={announce}
          onToggleAnnounce={() => {
            setPref('announce', !announce);
          }}
        />
      </div>
      {canControl && (
        <LiveControls
          lectureId={lectureId}
          status={status}
          paused={paused}
          stopped={stopped}
          deviceOnline={session.device?.online ?? null}
        />
      )}
      <div className={styles.banners}>
        <ConnectionBanner
          connection={connection}
          lectureId={lectureId}
          onResume={() => {
            manager.resume();
          }}
        />
        <WidgetErrorBoundary>
          <DeviceAlertBanner
            device={session.device}
            recording={status === 'recording'}
            isLecturer={isLecturer}
          />
        </WidgetErrorBoundary>
      </div>
      {status === 'scheduled' && (
        <EmptyState
          icon={Mic}
          title={isLecturer ? t('waiting.lecturerTitle') : t('waiting.studentTitle')}
          body={isLecturer ? t('waiting.lecturerBody') : t('waiting.studentBody')}
        />
      )}
      {ended && (
        <EmptyState
          icon={CircleCheck}
          title={t('ended.title')}
          body={session.notesReady.length > 0 ? t('ended.notesReady') : t('ended.body')}
          action={
            <ButtonLink variant="primary" to={paths.lecture(lectureId)}>
              {t('ended.open')}
            </ButtonLink>
          }
        />
      )}
      {canControl && lecture.data.device_id !== null && (
        <WidgetErrorBoundary>
          <DeviceMonitor deviceId={lecture.data.device_id} status={session.device} />
        </WidgetErrorBoundary>
      )}
      <div className={styles.layout}>
        <WidgetErrorBoundary>
          <LiveTranscriptPanel lectureId={lectureId} canEdit={canControl} />
        </WidgetErrorBoundary>
        <WidgetErrorBoundary>
          <LiveKeyTermsPanel />
        </WidgetErrorBoundary>
      </div>
    </>
  );
}

export function LiveLectureRoute() {
  const { lectureId } = useParams();
  if (lectureId === undefined) return null;
  // Keyed so switching lectures resets every live view state.
  return <LiveScreen key={lectureId} lectureId={lectureId} />;
}
