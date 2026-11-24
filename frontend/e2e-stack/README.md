# Full-stack E2E test

`npm run e2e` runs the UI against the mock API. This suite runs it against the **real** backend.
A lecturer prepares a lecture in the browser, and the device simulator streams a spoken WAV into
the gateway. The tests then check:

- The real ASR worker (faster-whisper) shows the transcript live.
- A 10 s network outage at 0:30 is backfilled with no duplicate lines.
- The finished lecture is searchable.
- The admin pages work.
- With `STACK_REAL_AI=1` (below), the generated notes can be published and used.

By default the notes worker runs **without** access to Bedrock. The test then expects the notes
tab to show its failure state, and the lecture must still complete.

With `STACK_REAL_AI=1`, the API and notes worker must have working Bedrock settings
(`BEDROCK_LLM_MODEL_ID`, `BEDROCK_EMBED_MODEL_ID`, AWS credentials). A fourth test then waits for
the draft notes, publishes them as the lecturer, and checks as a student that every study tab
loads and that Ask answers with jump links. These calls are billed to the AWS account.

## 1. Start the stack

You need PostgreSQL 16 with pgvector and Redis 5 or later. Redis must be 5+ because the API uses
Streams. The Windows "Redis 3.0" port will not work; use Memurai or WSL instead.

```bash
cd backend
pip install -e ".[dev,asr]"
alembic upgrade head
python -m tools.seed --admin-email admin@college.example --demo   # use one password for all three; note the device id/token
```

Run these in separate terminals. Set `COOKIE_SECURE=false` because this is plain HTTP, and keep
the default `ASR_MODEL=small` (see section 2):

```bash
uvicorn app.api.main:app --port 8000
uvicorn app.gateway.main:app --port 8001
python -m worker.asr.main
python -m worker.notes.main
cd ../frontend && npm run dev          # http://localhost:5173, proxies to :8000
```

## 2. Make a test recording

The test checks phrases from `e2e-stack/lecture.txt` (an English lecture of about 65 s on CPU scheduling
with a pause before "We measure"). On Windows, this speaks it with a built-in voice and writes a
16 kHz mono 16-bit WAV:

```powershell
powershell -ExecutionPolicy Bypass -File frontend/e2e-stack/make_recording.ps1 -Out lecture.wav
```

Elsewhere, record the text yourself at 16 kHz mono 16-bit, leaving a short pause at `[pause]`.

The default voice is US English (David), because the test checks exact phrases. With the Indian
English voices (`-Voice "Microsoft Heera"` or `"Microsoft Ravi"`), Whisper `small` writes
"convoy effect" as "convo effect", and `base` makes more such errors. Keep the default
`ASR_MODEL=small` for the ASR worker.

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
