import { zodResolver } from '@hookform/resolvers/zod';
import { Mic } from 'lucide-react';
import { useMemo } from 'react';
import { useForm } from 'react-hook-form';
import { useNavigate } from 'react-router';
import { useTranslation } from 'react-i18next';
import { z } from 'zod';
import type { Device, Subject } from '@/api/types';
import { paths } from '@/app/paths';
import { Button } from '@/components/Button';
import { Banner, ErrorBanner } from '@/components/Feedback';
import { Checkbox, Field, Select, TextInput } from '@/components/Field';
import { toast } from '@/components/Toast';
import { usePrepareLecture } from '@/features/lectures/api';
import styles from './Home.module.css';

function useSchema() {
  const { t } = useTranslation('lectures');
  return useMemo(
    () =>
      z.object({
        subject: z.string().trim().min(2, t('prepare.validation.subject')).max(120),
        title: z
          .string()
          .trim()
          .min(3, t('prepare.validation.titleShort'))
          .max(160, t('prepare.validation.titleLong')),
        language: z.enum(['en', 'te', 'hi']),
        device_id: z.string().min(1, t('prepare.validation.device')),
        consent_confirmed: z.literal(true, { error: t('prepare.validation.consent') }),
      }),
    [t],
  );
}

type FormValues = z.input<ReturnType<typeof useSchema>>;
type Values = z.output<ReturnType<typeof useSchema>>;

/** A typed subject that matches an existing one (code, name or "CODE · Name") uses its id;
 * anything else is sent as a name and the server reuses or creates the subject. */
function subjectField(typed: string, subjects: readonly Subject[]) {
  const key = typed.trim().toLowerCase();
  const match = subjects.find(
    (s) =>
      s.code.toLowerCase() === key ||
      s.name.toLowerCase() === key ||
      `${s.code} · ${s.name}`.toLowerCase() === key,
  );
  return match !== undefined ? { subject_id: match.id } : { subject_name: typed.trim() };
}

interface PrepareLectureFormProps {
  subjects: readonly Subject[];
  devices: readonly Device[];
}

export function PrepareLectureForm({ subjects, devices }: PrepareLectureFormProps) {
  const { t } = useTranslation('lectures');
  const { t: tc } = useTranslation();
  const navigate = useNavigate();
  const prepare = usePrepareLecture();
  const schema = useSchema();
  const usable = devices.filter((d) => !d.recording);
  const { register, handleSubmit, formState } = useForm<FormValues, unknown, Values>({
    resolver: zodResolver(schema),
    mode: 'onTouched',
    defaultValues: {
      subject: '',
      title: '',
      language: 'en',
      device_id: usable.length === 1 ? (usable[0]?.id ?? '') : '',
    },
  });
  const errors = formState.errors;

  const onSubmit = ({ subject, ...rest }: Values): void => {
    prepare.mutate(
      { ...rest, ...subjectField(subject, subjects) },
      {
        onSuccess: (lecture) => {
          toast.success(t('prepare.success'));
          void navigate(paths.live(lecture.id));
        },
      },
    );
  };

  if (devices.length === 0) return <Banner tone="info">{t('prepare.noDevices')}</Banner>;

  return (
    <form className={styles.form} noValidate onSubmit={(e) => void handleSubmit(onSubmit)(e)}>
      {prepare.isError && <ErrorBanner title={t('prepare.errors.failed')} error={prepare.error} />}
      <Field
        label={t('prepare.subject')}
        hint={t('prepare.subjectHint')}
        error={errors.subject?.message}
      >
        {(c) => (
          <>
            <TextInput
              {...c}
              {...register('subject')}
              list="prepare-subjects"
              placeholder={t('prepare.subjectPlaceholder')}
              autoComplete="off"
            />
            <datalist id="prepare-subjects">
              {subjects.map((s) => (
                <option key={s.id} value={`${s.code} · ${s.name}`} />
              ))}
            </datalist>
          </>
        )}
      </Field>
      <Field
        label={t('prepare.titleLabel')}
        hint={t('prepare.titleHint')}
        error={errors.title?.message}
      >
        {(c) => (
          <TextInput
            {...c}
            {...register('title')}
            placeholder={t('prepare.titlePlaceholder')}
            autoComplete="off"
          />
        )}
      </Field>
      <div className={styles.row}>
        <Field label={t('prepare.language')} hint={t('prepare.languageHint')}>
          {(c) => (
            <Select {...c} {...register('language')}>
              {(['en', 'te', 'hi'] as const).map((code) => (
                <option key={code} value={code}>
                  {tc(`language.${code}`)}
                </option>
              ))}
            </Select>
          )}
        </Field>
        <Field label={t('prepare.device')} error={errors.device_id?.message}>
          {(c) => (
            <Select {...c} {...register('device_id')}>
              <option value="">{t('prepare.devicePlaceholder')}</option>
              {devices.map((d) => (
                <option key={d.id} value={d.id} disabled={d.recording}>
                  {d.recording
                    ? t('prepare.deviceBusy', { name: d.name })
                    : d.online
                      ? d.name
                      : t('prepare.deviceOffline', { name: d.name })}
                </option>
              ))}
            </Select>
          )}
        </Field>
      </div>
      <Checkbox
        label={t('prepare.consent')}
        hint={t('prepare.consentHint')}
        {...register('consent_confirmed')}
      />
      {errors.consent_confirmed?.message !== undefined && (
        <p className={styles.error} role="alert">
          {errors.consent_confirmed.message}
        </p>
      )}
      <Button type="submit" variant="primary" icon={Mic} loading={prepare.isPending}>
        {prepare.isPending ? t('prepare.submitting') : t('prepare.submit')}
      </Button>
    </form>
  );
}
