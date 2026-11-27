import { zodResolver } from '@hookform/resolvers/zod';
import { useMemo } from 'react';
import { useForm } from 'react-hook-form';
import { useTranslation } from 'react-i18next';
import { z } from 'zod';
import type { Device } from '@/api/types';
import { Button } from '@/components/Button';
import { ErrorBanner } from '@/components/Feedback';
import { Field, TextInput } from '@/components/Field';
import { Section } from '@/components/Page';
import { toast } from '@/components/Toast';
import { useChangeWifi } from './api';
import styles from './Devices.module.css';

/** Lecturer/admin: move the recorder to another Wi-Fi network without the setup hotspot. */
export function DeviceWifiForm({ device }: { device: Device }) {
  const { t } = useTranslation('devices');
  const change = useChangeWifi(device.id);
  const schema = useMemo(
    () =>
      z.object({
        ssid: z
          .string()
          .refine((v) => v.trim() !== '', t('wifi.ssidRequired'))
          .refine((v) => v.length <= 32, t('wifi.ssidTooLong')),
        password: z
          .string()
          .refine((v) => v === '' || (v.length >= 8 && v.length <= 63), t('wifi.passwordLength')),
      }),
    [t],
  );
  const form = useForm<z.infer<typeof schema>>({
    resolver: zodResolver(schema),
    defaultValues: { ssid: '', password: '' },
  });
  const errors = form.formState.errors;
  const blocked = !device.online
    ? t('wifi.offline')
    : device.recording || device.current_lecture_id !== null
      ? t('wifi.recording')
      : null;
  return (
    <Section title={t('wifi.heading')} headingId="device-wifi">
      <form
        className={styles.formGrid}
        noValidate
        onSubmit={(e) =>
          void form.handleSubmit((values) => {
            change.mutate(values, {
              onSuccess: () => {
                toast.success(t('wifi.sent', { ssid: values.ssid }));
                form.reset();
              },
            });
          })(e)
        }
      >
        <p className={styles.muted}>{t('wifi.intro')}</p>
        {change.isError && <ErrorBanner title={t('wifi.error')} error={change.error} />}
        <Field label={t('wifi.ssid')} error={errors.ssid?.message}>
          {(c) => <TextInput {...c} autoComplete="off" {...form.register('ssid')} />}
        </Field>
        <Field
          label={t('wifi.password')}
          hint={t('wifi.passwordHint')}
          error={errors.password?.message}
        >
          {(c) => (
            <TextInput
              {...c}
              type="password"
              autoComplete="new-password"
              {...form.register('password')}
            />
          )}
        </Field>
        {blocked !== null && <p className={styles.muted}>{blocked}</p>}
        <div>
          <Button
            type="submit"
            variant="primary"
            loading={change.isPending}
            disabled={blocked !== null}
          >
            {t('wifi.submit')}
          </Button>
        </div>
      </form>
    </Section>
  );
}
