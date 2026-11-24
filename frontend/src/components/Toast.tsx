import * as RadixToast from '@radix-ui/react-toast';
import { CircleAlert, CircleCheck, Info, X } from 'lucide-react';
import { createPortal } from 'react-dom';
import { useTranslation } from 'react-i18next';
import { create } from 'zustand';
import { cx } from '@/lib/cx';
import { IconButton } from './Button';
import styles from './Overlay.module.css';

type ToastTone = 'success' | 'error' | 'info';

interface ToastItem {
  id: number;
  tone: ToastTone;
  title: string;
  description?: string | undefined;
}

interface ToastState {
  toasts: ToastItem[];
  push: (tone: ToastTone, title: string, description?: string) => void;
  dismiss: (id: number) => void;
}

let nextId = 1;

/** Global, ephemeral UI notifications (truly global client state → Zustand). */
export const useToastStore = create<ToastState>()((set) => ({
  toasts: [],
  push: (tone, title, description) => {
    const id = nextId;
    nextId += 1;
    set((state) => ({ toasts: [...state.toasts.slice(-2), { id, tone, title, description }] }));
  },
  dismiss: (id) => {
    set((state) => ({ toasts: state.toasts.filter((toast) => toast.id !== id) }));
  },
}));

export const toast = {
  success: (title: string) => {
    useToastStore.getState().push('success', title);
  },
  /** `description` says why it failed and what to do. */
  error: (title: string, description?: string) => {
    useToastStore.getState().push('error', title, description);
  },
  info: (title: string) => {
    useToastStore.getState().push('info', title);
  },
};

const ICONS = { success: CircleCheck, error: CircleAlert, info: Info } as const;
const TONE_CLASS = {
  success: styles.toastSuccess,
  error: styles.toastError,
  info: styles.toastInfo,
} as const;

export function Toaster() {
  const { t } = useTranslation();
  const toasts = useToastStore((state) => state.toasts);
  const dismiss = useToastStore((state) => state.dismiss);
  return (
    <RadixToast.Provider swipeDirection="down" duration={5000}>
      {toasts.map((item) => {
        const Icon = ICONS[item.tone];
        return (
          <RadixToast.Root
            key={item.id}
            className={cx(styles.toast, TONE_CLASS[item.tone])}
            type={item.tone === 'error' ? 'foreground' : 'background'}
            // Errors carry a reason to read: give them longer on screen.
            duration={item.tone === 'error' ? 10_000 : undefined}
            onOpenChange={(open) => {
              if (!open) dismiss(item.id);
            }}
          >
            <Icon aria-hidden="true" />
            <div className={styles.toastText}>
              <RadixToast.Title className={styles.toastTitle}>{item.title}</RadixToast.Title>
              {item.description !== undefined && (
                <RadixToast.Description className={styles.toastDescription}>
                  {item.description}
                </RadixToast.Description>
              )}
            </div>
            <RadixToast.Close asChild>
              <IconButton icon={X} label={t('actions.close')} />
            </RadixToast.Close>
          </RadixToast.Root>
        );
      })}
      {/* In <body>, not #root: #root is its own stacking context (isolation), so a toast inside
          it would sit under an open dialog, e.g. the error after a failed "Delete" confirm. */}
      {createPortal(<RadixToast.Viewport className={styles.toastViewport} />, document.body)}
    </RadixToast.Provider>
  );
}
