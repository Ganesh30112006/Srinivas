# Build Progress

The build follows the module order of the system brief, with the decisions in `DECISIONS.md` applied throughout.

| # | Module | Status | Verified here |
|---|---|---|---|
| 1 | Hardware validation firmware (`firmware/mic_test`, `firmware/tools`) | done | no (needs ESP-IDF + board) |
| 2 | Contracts (`contracts/`) | done | JSON Schemas checked by backend tests; `openapi.json` exported from FastAPI |
| 3 | Device firmware (`firmware/recorder`, `firmware/lc_core`) | done | `lc_core` host tests written, not compiled (no C compiler here); ADPCM vectors cross-checked from Python |
| 4 | Device gateway + simulator | done | pytest: real WebSocket + embedded PostgreSQL; simulator E2E with an outage |
| 5 | ASR worker | done | pytest with a fake transcriber; **real faster-whisper (`base`, CPU int8) run end to end on 2026-11-23** with a synthetic spoken lecture (see below) |
| 6 | API + database | done | pytest; Alembic upgrade/downgrade on an embedded database |
| 7 | Notes / Q&A workers | done | pytest with a scripted LLM (Bedrock not called here) |
| 8 | Frontend (`frontend/`) | done | `npm run check`, build + bundle budget, Playwright E2E incl. axe (mock API), and the full-stack Playwright suite against the real backend |
| 9 | Deployment, no Docker (`infra/`) | done | `bash -n` on the scripts; Caddyfile reviewed by hand (no Caddy binary here; `setup.sh` runs `caddy validate` before reloading) |
| 10 | Evaluation scripts (`eval/`) | done | 38 pytest tests for the metric and report functions; CLIs smoke-tested with `--help` |

## Master-plan additions (2026-11-19/20)

Agreed with the owner on 2026-11-19. Deliberate deviation: no MicroSD; internal flash + PSRAM give about 12.7 min of offline audio.

| Addition | Where | Verified here |
|---|---|---|
| Web Start/Pause/Resume/Stop + physical Pause/Resume button (GPIO7), lecture `paused` flag | gateway, `firmware/recorder`, Live page | pytest (`test_master_plan.py`); firmware not compiled |
| Live device monitoring: mic level, mic status, pending chunks, sync progress | gateway status, Live page, device page | pytest, vitest |
| Manual transcript correction | API + Transcript/Live pages | pytest, vitest |
| Flashcards and revision notes | notes worker, API, Flashcards + Revision tabs | pytest, vitest |
| Settings: profile, password, notifications, server info (admin) | API + Settings page | pytest, vitest |
| Device settings (name, room, gain, voice detection) and lecturer assignment | API + device page (admin) | pytest, vitest |
| Configurable audio retention (`AUDIO_RETENTION_DAYS`, `python -m tools.retention`) | backend tool | pytest |
| Phase 12 documents: report, PPT outline, UML, circuit, user manual, demo script | `docs/project/` | reviewed against the code; results tables left *to be measured* |

## Quality gates (all passing on 2026-11-23)

| Part | Gate | Result |
|---|---|---|
| Backend | `pytest`, `ruff check`, `ruff format --check`, `mypy app worker tools` (strict) | 73 tests; clean |
| Eval | `pytest eval`, ruff, `mypy` with the backend on `MYPYPATH` (see `eval/README.md`) | 38 tests; clean |
| Frontend | `npm run check` (tsc ×2, eslint, CSS tokens, Prettier, vitest) | 105 tests; clean |
| Frontend | `npm run build && npm run size` | 183.3 KB initial JS gzip (budget 200 KB), no mock code |
| Frontend | `npm run e2e` (`PW_CHANNEL=chrome` here) | 34 tests against the mock API, incl. the axe sweep of every page and role |
| Full stack | `npm run e2e:stack` (`frontend/e2e-stack/README.md`) | 3 tests against the real API, gateway, ASR worker (faster-whisper) and notes worker, with the device simulator and a 10 s outage; fails on any console error or unexpected 4xx/5xx |

## Fixed while adding tests and deployment (2026-11-19)

- **Transcript stopped loading after page 2** when a response was fast. `useFullTranscript` now also depends on the page count.
- **The transcript search box dropped keystrokes** because it was driven by the URL. That would also break Hindi/Telugu input methods. The query now lives in state and is mirrored to `?q=`.
- **The API client captured `fetch` at import time**, so request interceptors could not see its requests. It now resolves `fetch` on each call.
- **`render.ack` was sent only for finals and backfills.** Partials are now acknowledged too, and `eval/` pairs each chunk with the first *rendered* window that covers it.
- **Lost work after a Redis flush or a worker crash:**
  - the ASR worker re-queues `recording` lectures on start;
  - the notes worker re-queues `processing` lectures whose job is missing (`BLPOP` removes the job before it runs).
- **Accessibility, found by axe:**
  - the account button's spoken name didn't match its visible text (WCAG 2.5.3);
  - the scrollable key-terms panel couldn't be focused (WCAG 2.1.1).
- **New tool:** `tools.register_device`, which registers a microphone from the server shell and assigns it to lecturers. (Since 2026-11-20 the device page can assign lecturers too.)
- **Tooling fixes:**
  - husky was pointing at a hooks path that doesn't exist; it now uses `frontend/.husky` (pre-commit runs lint-staged);
  - ESLint now covers config and e2e files;
  - Prettier has been applied to all of `frontend/`.

## Fixed on 2026-11-20

- **Flaky frontend tests under full-suite load:** Testing Library's 1 s wait was too short when all files run in parallel. `asyncUtilTimeout` is now 5 s, the per-test timeout 15 s, and the Library test waits for the lecture count instead of reading it once.
- **Backend tests could not start** because a test PostgreSQL from an earlier run was still holding `%TEMP%\smartclass_test_pg`. Stopping that process and deleting the folder fixes it.

## Fixed on 2026-11-23 (found by running the real stack with Playwright)

The whole system was run locally for the first time: embedded PostgreSQL, a Redis-compatible
server, the API, gateway, ASR worker (real faster-whisper `base`) and notes worker, the Vite dev
server, and the device simulator streaming an 80 s spoken lecture with a 10 s network outage.

- **The notes worker crashed every few seconds.** redis-py 8 defaults `socket_timeout` to 5 s, which
  killed its 5 s `BLPOP` just before Redis answered, so no notes would ever have been generated.
  The shared client now sets a 30 s socket timeout (`app/events.py`, `tests/test_events.py`).
- **Doubled and stray words at window cuts** ("for For example", "too many Any", "class as",
  "we we"). Whisper's word end times are early, so each cut split a word. Each decode now re-hears
  1 s of committed audio. Where the cut is the end of a committed word, a word counts as new only
  if most of it lies after the cut, and an exact 1-5 word repeat or a short unsure fragment at the
  cut is dropped. Where silence moved the cut instead, none of this applies (applying it there
  lost the first word of "We measure" in two live runs). A word straddling the cut is judged by
  its text: it is dropped only if it sounds like the last committed word ("wait" / "weight").
- **Whisper hallucinations on near-silent fragments** ("You", "I'll see you in the next video")
  are discarded when a segment is unsure on all three signals (log-probability, no-speech,
  word probability); thresholds come from measured segments (`worker/asr/transcriber.py`).
- **Lost and invented speech from Whisper's text prompt.** The worker prompted each decode with
  the previous 200 characters of transcript. On short windows Whisper then repeated the prompt
  ("... several processes are ready to run. In a multi-programmed system ...") or degenerated on
  every fallback attempt ("乾 乾 乾", once "邸rup 邶After"), and the window's real speech was lost,
  while the same audio without a prompt decoded cleanly. Decodes are no longer prompted (the 1 s
  of context audio gives continuity). As backstops: a fallback result that still scores below -1
  log-probability is dropped, a phrase repeated 3+ times in a row is collapsed, and words in
  scripts a lecture cannot contain (anything but Latin, Telugu, Devanagari) are dropped.
- **The gateway's shutdown could hang forever** (2 of 4 full backend runs timed out in a test's
  teardown). A device connection's cleanup awaited `UNSUBSCRIBE` on a Redis connection whose read
  had just been cancelled, and the reply never came. That cleanup is now bounded (2 s), and so is
  the wait for all cleanups at shutdown (10 s), so `systemctl stop` can no longer stall on it.
  Afterwards: 5 of 5 full runs passed.
- **Students were told notes were "still being generated" after generation had failed.** The API
  now answers `404 *_failed` (not `*_not_ready`); the page says it couldn't be generated and gives
  the lecturer a Regenerate button.
- **The live page kept showing "Live · Delay 2 ms" after the lecture completed** (the server closes
  the socket up to 5 s later). The header now follows the lecture status at once.
- **"47 matches in 1 lectures"** on the search page: the two counts are now pluralised separately.
- Plain-UI polish: the sticky bars on the live and transcript pages line up with the content on
  desktop; meter bars are the same length whatever the percentage; "No signal" shows a Wi-Fi-off
  icon instead of a lone dot; screenshots wait for the transcript to finish loading.
- Docs: test counts and missing test rows in `backend/README.md`, the eval type-check command,
  and the streaming-recognition section of `docs/project/REPORT.md`.

How it was verified: besides the full-stack suite, the real ASR processor was driven offline
with real faster-whisper over the recording, chunk by chunk with the outage, logging every
decode; that is how the prompt, loop and seam causes above were found. With all fixes, that run
and three consecutive full-stack runs (9/9 tests) produced the complete lecture with every
sentence, no duplicates, loops, fragments or foreign-script junk. Remaining differences are word
errors of the `base` model ("queue" heard as "Q", "ready queue" once as "reddq").

Not changed, by design: a worker that loses Redis exits and is restarted by systemd
(`Restart=always`); both workers re-queue unfinished lectures on start.

## Not verified here (needs real hardware or services)

- Firmware build and on-device behaviour. The first ESP-IDF build will be the first compile.
- faster-whisper accuracy on real classrooms: it now runs end to end here on synthetic English speech (`base` model). Measure WER and speed with `eval/asr_benchmark.py` on real recordings, including Telugu and Hindi, and choose `ASR_MODEL` from that. `base` with greedy live decoding still misrecognises some words ("ready queue" → "reddq").
- Amazon Bedrock calls: model access and IAM.
- `infra/ec2/setup.sh` on a real Ubuntu 24.04 instance, and Caddy obtaining a certificate.

## Plain-UI restyle

Done (2026-11-20, polished 2026-11-23): white background, one blue accent, system font, smaller
controls, no pills, wrapping tabs, no horizontal overflow at 375 px or 1280 px.
