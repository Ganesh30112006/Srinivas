import { describe, expect, it } from 'vitest';
import { applyMessage, EMPTY_SESSION, elapsedMs } from './liveSession';
import type { SequencedServerMessage } from './messages';

type StatusMessage = Extract<SequencedServerMessage, { type: 'lecture.status' }>;

function status(fields: Partial<StatusMessage>): StatusMessage {
  return {
    type: 'lecture.status',
    seq: 1,
    server_ts: 0,
    status: 'recording',
    duration_ms: 10_000,
    ...fields,
  };
}

describe('lecture.status: stopped', () => {
  it('marks the session stopped as soon as the device confirms Stop', () => {
    const recording = applyMessage(EMPTY_SESSION, status({}), 0);
    expect(recording.stopped).toBe(false);
    const stopped = applyMessage(recording, status({ stopped: true }), 0);
    expect(stopped.lectureStatus).toBe('recording');
    expect(stopped.stopped).toBe(true);
  });

  it('keeps stopped through later duration updates and clears it once processing', () => {
    let session = applyMessage(EMPTY_SESSION, status({ stopped: true }), 0);
    session = applyMessage(session, status({ duration_ms: 12_000 }), 0);
    expect(session.stopped).toBe(true);
    session = applyMessage(session, status({ status: 'processing' }), 0);
    expect(session.stopped).toBe(false);
  });

  it('stops the elapsed clock once stopped', () => {
    const session = applyMessage(EMPTY_SESSION, status({ stopped: true }), 1_000);
    expect(elapsedMs(session, 60_000)).toBe(10_000);
  });
});
