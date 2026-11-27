import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import type { LectureStatusValue } from '@/api/ws/messages';
import { toast } from '@/components/Toast';
import { usePrefs } from '@/features/settings/prefsStore';

/**
 * Live-page notifications (opt-out in Settings): reconnected, lecture ended, notes ready.
 * Toasts are side effects of state changes, not state.
 */
export function useLiveToasts(
  connectionStatus: string,
  lectureStatus: LectureStatusValue | null,
  notesReadyCount: number,
): void {
  const { t } = useTranslation('live');
  const notifyConnection = usePrefs((s) => s.notifyConnection);
  const notifyLectureEnded = usePrefs((s) => s.notifyLectureEnded);
  const notifyNotesReady = usePrefs((s) => s.notifyNotesReady);
  const previous = useRef({
    connection: connectionStatus,
    wasLive: false,
    status: lectureStatus,
    notes: 0,
  });
  useEffect(() => {
    const before = previous.current;
    if (
      connectionStatus === 'live' &&
      before.wasLive &&
      before.connection !== 'live' &&
      notifyConnection
    )
      toast.info(t('toasts.reconnected'));
    const ended = (s: string | null): boolean => s === 'processing' || s === 'completed';
    if (ended(lectureStatus) && before.status === 'recording' && notifyLectureEnded)
      toast.info(t('toasts.ended'));
    if (notesReadyCount > 0 && before.notes === 0 && notifyNotesReady)
      toast.success(t('toasts.notesReady'));
    previous.current = {
      connection: connectionStatus,
      wasLive: before.wasLive || connectionStatus === 'live',
      status: lectureStatus,
      notes: notesReadyCount,
    };
  }, [
    connectionStatus,
    lectureStatus,
    notesReadyCount,
    notifyConnection,
    notifyLectureEnded,
    notifyNotesReady,
    t,
  ]);
}
