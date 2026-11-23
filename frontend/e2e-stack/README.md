# Full-stack E2E test

`npm run e2e` runs the UI against the mock API. This suite runs it against the **real** backend.
A lecturer prepares a lecture in the browser, and the device simulator streams a spoken WAV into
the gateway. The test then checks four things:

- The real ASR worker (faster-whisper) shows the transcript live.
- A 10 s network outage at 0:30 is backfilled with no duplicate lines.
- The finished lecture is searchable.
- The admin pages work.

The notes worker is meant to run **without** access to Bedrock. The test then expects the notes
tab to show its failure state, and the lecture must still complete.

## 1. Start the stack

You need PostgreSQL 16 with pgvector and Redis 5 or later. Redis must be 5+ because the API uses
Streams. The Windows "Redis 3.0" port will not work; use Memurai or WSL instead.

```bash
cd backend
pip install -e ".[dev,asr]"
alembic upgrade head
python -m tools.seed --admin-email admin@college.example --demo   # use one password for all three; note the device id/token
```

Run these in separate terminals. Set `COOKIE_SECURE=false` because this is plain HTTP, and set
`ASR_MODEL=base` for speed:

```bash
uvicorn app.api.main:app --port 8000
uvicorn app.gateway.main:app --port 8001
python -m worker.asr.main
python -m worker.notes.main
cd ../frontend && npm run dev          # http://localhost:5173, proxies to :8000
```

## 2. Make a test recording

The test expects an English lecture of about 80 s on CPU scheduling. It must mention "convoy
effect" and "round robin" and end with "multilevel feedback queues". The WAV must be 16 kHz mono
16-bit. On Windows, the System.Speech synthesizer can write it directly:
`SpeechAudioFormatInfo(16000, Sixteen, Mono)`.

## 3. Run

```bash
STACK_PASSWORD=<seed password> \
STACK_DEVICE_ID=<device id> STACK_DEVICE_TOKEN=<device token> \
STACK_WAV=/path/lecture.wav \
STACK_PYTHON=../backend/.venv/bin/python STACK_BACKEND_DIR=../backend \
npm run e2e:stack
```

Add `PW_CHANNEL=chrome` to use the system Chrome. The suite fails on any browser console error,
uncaught exception, or 5xx response.
