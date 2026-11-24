# Build Progress

The build follows the module order of the system brief, with the decisions in `DECISIONS.md` applied throughout.

| # | Module | Status | Verified here |
|---|---|---|---|
| 1 | Hardware validation firmware (`firmware/mic_test`, `firmware/tools`) | done | compiled and linked for the ESP32-S3 against ESP-IDF 5.5 (`firmware/tools/compile_check.py`); not run on a board |
| 2 | Contracts (`contracts/`) | done | JSON Schemas checked by backend tests; `openapi.json` exported from FastAPI |
| 3 | Device firmware (`firmware/recorder`, `firmware/lc_core`) | done | `lc_core` host tests: 80 checks pass (zig cc, also under UBSan); recorder compiled with `-Werror`, linked against ESP-IDF 5.5 and turned into a valid 1.05 MB image (2026-11-24); not run on a board |
| 4 | Device gateway + simulator | done | pytest: real WebSocket + embedded PostgreSQL; simulator E2E with an outage |
| 5 | ASR worker | done | pytest with a fake transcriber; real faster-whisper end to end (`base` on 2026-11-23, `small` on 2026-11-24); WER measured on 21.5 min of synthetic speech (see below) |
| 6 | API + database | done | pytest; Alembic upgrade/downgrade on an embedded database |
| 7 | Notes / Q&A workers | done | pytest with a scripted LLM; real Amazon Bedrock in the full-stack suite and on Telugu/Hindi code-switched transcripts (2026-11-24) |
| 8 | Frontend (`frontend/`) | done | `npm run check`, build + bundle budget, Playwright E2E incl. axe (mock API), and the full-stack Playwright suite against the real backend |
| 9 | Deployment, no Docker (`infra/`) | done | `bash -n` on the scripts; Caddyfile reviewed by hand (no Caddy binary here; `setup.sh` runs `caddy validate` before reloading) |
| 10 | Evaluation scripts (`eval/`) | done | 39 pytest tests; all three reports run on real data from the local stack (2026-11-24) |

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
| Configurable audio retention (`AUDIO_RETENTION_DAYS`, `python -m tools.retention`) | backend tool, daily `smartclass-retention.timer` | pytest; timer not run (no systemd here) |
| Phase 12 documents: report, PPT outline, UML, circuit, user manual, demo script | `docs/project/` | reviewed against the code; results tables left *to be measured* |

## Quality gates (all passing on 2026-11-24)

| Part | Gate | Result |
|---|---|---|
| Backend | `pytest`, `ruff check`, `ruff format --check`, `mypy app worker tools` (strict) | 90 tests; clean |
| Eval | `pytest eval`, ruff, `mypy` with the backend on `MYPYPATH` (see `eval/README.md`) | 39 tests; clean |
| Frontend | `npm run check` (tsc ×2, eslint, CSS tokens, Prettier, vitest) | 119 tests; clean |
| Frontend | `npm run build && npm run size` | 184.6 KB initial JS gzip (budget 200 KB), no mock code |
| Firmware | `firmware/tools/compile_check.py --host-tests ...` | 17 files with `-Werror`, both apps linked, image 1.05 MB / 3 MB, 80 host checks |
| Frontend | `npm run e2e` (`PW_CHANNEL=chrome` here) | 34 tests against the mock API, incl. the axe sweep of every page and role |
| Full stack | `npm run e2e:stack` (`frontend/e2e-stack/README.md`) | 4 tests against the real API, gateway, ASR worker (faster-whisper `small`) and notes worker with real Amazon Bedrock (`STACK_REAL_AI=1`); without AI: 3 passed, 1 skipped. Device simulator with a 10 s outage; fails on any console error or unexpected 4xx/5xx |

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

## Fixed later on 2026-11-23 (found with real Amazon Bedrock and a review pass)

Bedrock was called for the first time, in ap-south-1. `apac.amazon.nova-lite-v1:0` works on any
account. Claude and the Cohere embedding model are sold through AWS Marketplace and need a valid
payment method on the account, so the backend now also supports Titan embeddings.

- **Titan embeddings:** setting `BEDROCK_EMBED_MODEL_ID=amazon.titan-embed-text-v2:0` selects a
  Titan embedder at the database's 1024 dimensions (`app/ai/client.py`, `tests/test_ai_client.py`).
  Cohere stays the default because it is better for Hindi and Telugu.
- **Ask lost all its citations with Nova.** The model cited lines by their index ("3", "5"), not by
  segment id, so every jump link was dropped. The prompt now asks for indexes, and both forms are
  accepted. Ask also sends the line before and after each search hit: lecturers often define a term
  in the next sentence ("This is called the convoy effect."), and without it the answer came from
  the wrong lines.
- **Notes repeated themselves.** Nova returned overlapping topics, so shared lines were written up
  twice; topics are now made contiguous. It also started section bodies with the section's own
  heading as an `#` h1. That heading is removed, and the page renders any heading inside a body two
  levels down, so a body can never outrank the lecture title.
- **Prompts:** the model may correct an obvious misrecognised term ("feedback cues" → "feedback
  queues") but must not add examples, numbers or tables the lecturer didn't give. `prompt_version`
  is now `2026-11-23.1`.
- **Security and validation:**
  - sign-in always runs the password hash, so the response time no longer reveals whether an
    account exists;
  - an admin can't remove their own admin role;
  - unknown, student or duplicate lecturer ids on a device are a 422, not a database error (500);
  - names, titles, questions and transcript edits are trimmed, so "   " is rejected, not saved;
  - signing in clears the previous user's cached pages (an expired session on a shared lab PC
    could otherwise show a lecturer's draft notes to the next student).
- **Live socket:** a browser that leaves before subscribing no longer logs an error.
- **Audio retention now runs on its own:** `smartclass-retention.timer` runs `tools.retention`
  daily at 03:30. `setup.sh` enables it, and `deploy.sh` enables it on servers set up earlier.
- **Full-stack test with real AI:** with `STACK_REAL_AI=1`, a fourth test publishes the generated
  notes and uses every study tab and Ask as a student (`frontend/e2e-stack/README.md`).
- The `lc_core` host-test header uses line comments: the `src/*.c` glob inside a block comment
  trips `-Wcomment` under `-Werror`.

## Clear error pages and messages (2026-11-24)

Every failure now says what failed, why, and what to do. Before, most error panels gave only a
title ("Couldn't load this lecture"), and several actions failed silently.

- **One explanation for every error** (`src/api/describeError.ts`). The server's own message is
  shown when it sent one, because those are written for users. Otherwise the explanation depends on
  the kind of failure: offline, server unreachable, session ended, no permission, not found,
  changed meanwhile, too many requests, timeout, server unavailable or restarting, server error,
  or a bug in the app. A short code ("Error code: device_busy · 409") helps an administrator.
- **Error panels** on every page and tab show that reason and code. Retry appears only where it can
  help, so not for "no permission" or "not found". The route error page adds "Go to home". Error
  pages: page not found, no access, new version available, and a last-resort "Something went
  wrong" screen around the router, so a crash outside a route no longer leaves a blank page.
- **Actions that failed silently** now show an error toast that names the action and gives the
  reason. These are: confirming consent, deleting a lecture, publishing or regenerating notes,
  changing a user, recording controls, export, and sign-out. Toasts render in `<body>`, so they
  also appear above an open confirm dialog.
- **Form errors** (add user or subject, register or update a device, prepare a lecture, correct a
  line, profile, password, notes editor, Ask) name the action, then the reason.
- **Ended sessions:** when a 401 ends a signed-in session, the sign-in page says "Your session has
  ended". A wrong password does not trigger this.
- **Backend:** an unexpected crash answers `500 internal_error`, and an unreachable PostgreSQL or
  Redis answers `503 service_unavailable`, both with a plain message, instead of a plain-text
  "Internal Server Error". A live socket is closed with 1011, so the browser reconnects.
  Validation errors name the field: "Title: should have at least 3 characters".
- Tests: `tests/test_errors.py` (backend), `describeError.test.ts` and `errorPages.test.tsx`
  (frontend).

## Firmware build, measurements and real AI (2026-11-24)

**Firmware compiled for the first time.** The ESP32-S3 compiler and ESP-IDF 5.5 libraries from the
Arduino IDE's esp32 package are on this machine:
- all 17 firmware C files compile with `-Wall -Wextra -Werror`, with no warnings;
- `recorder` and `mic_test` link against ESP-IDF;
- the recorder image is valid at 1.05 MB of the 3 MB partition;
- the `lc_core` host tests pass (80 checks, also under UBSan).

Repeat with `python firmware/tools/compile_check.py --host-tests "python -m ziglang cc"`
(`firmware/README.md` §6). Still not run on the board.

**Measurements** (synthetic speech and a local stack; see `docs/project/REPORT.md` §9):
- **ASR accuracy:** 16 synthetic recordings (US and Indian English voices, 21.5 min). Whisper `small`
  has 1.8% WER live and runs at a real-time factor of 0.21 on an i7-1255U; `base` has 2.5% at 0.075.
  The Indian-English voices score 2.0% (US 1.7%). The generator is reproducible:
  `eval/synthetic/make_lectures.ps1`.
- **Latency:** in a full local run with `small`, text first appears 9.3 s after speech (p50; p95
  23.8 s). Decoding on the laptop CPU, about 4.6 s per window, is the bottleneck. The network and
  browser stages take under 1 s together.
- **Recovery:** after outages of 10 s, 30 s, 2 min and 10 min, every chunk was stored exactly once
  and every gap was filled and transcribed.

**Real AI (Amazon Bedrock, ap-south-1):**
- The full-stack suite passes with `STACK_REAL_AI=1` (4/4): notes are generated, published and
  used, and Ask answers with jump links.
- Code-switched Telugu-English and Hindi-English transcripts produce English notes, quizzes and
  flashcards. English questions find the right Telugu/Hindi line in the top 3 every time (4/4,
  Titan embeddings).
- **Model choice:** `BEDROCK_LLM_MODEL_ID` now takes an ordered list, and each request falls back to
  the next model when one is refused or its reply fails validation. Configured:
  `global.anthropic.claude-opus-4-6-v1,apac.amazon.nova-lite-v1:0`. Claude Opus 4.6 is the best
  model this account has enabled, but AWS currently refuses it with `INVALID_PAYMENT_INSTRUMENT`
  (AWS Marketplace needs a valid payment method), so Nova Lite answered. Claude takes over without a
  config change once billing is fixed. Nova Pro and Nova 2 Lite were tried and returned invalid
  output; Cohere embeddings are blocked by the same billing issue, so Titan is used.

**Bugs found and fixed:**
- Model replies with raw line breaks inside JSON strings were rejected (Nova on the Telugu
  lecture). They are now accepted (`json.loads(strict=False)`).
- Short lectures were split into too many topics: the prompt always asked for 4–10. It now asks for
  about one topic per 4 minutes (a 1-minute Hindi lecture went from 6 sections to 1).
- Key terms had singular/plural duplicates ("Page"/"Pages") and terms the lecturer never said.
  Terms are now merged by singular form, and the prompt allows only terms used in the lines.
  `prompt_version` is `2026-11-24.1`.
- `eval/recovery_report.py` failed every fully processed lecture. It accepted only gaps in state
  `filled`, but `done` (filled and transcribed) comes after it.
- The full-stack transcript test read only the rows the virtual list had rendered, so the end of a
  longer lecture was never checked. The test recording is now reproducible:
  `frontend/e2e-stack/lecture.txt` and `make_recording.ps1`.
- `test_gateway_flow::test_live_first_resend_fills_gap_without_duplicates` failed once when the
  backend and frontend suites ran at the same time on this laptop. It passed 3 of 3 times alone, and
  the full suite passed 90/90 alone. It is timing-sensitive under heavy CPU load; run the suites
  one after the other.
- This machine's backend virtualenv was an editable install made before `worker/` and `tools/`
  existed, so `asr_benchmark.py` could not import them. A fresh install fixes it; the packaging was
  already correct.

## Not verified here (needs real hardware or services)

- On-device firmware behaviour, and a build with `idf.py` using the project's own `sdkconfig.defaults` (the check above used Arduino's prebuilt ESP-IDF libraries).
- ASR accuracy on real classroom audio, Telugu and Hindi: measured only on synthetic English speech so far (`small` 1.8% WER). Measure real recordings with `eval/asr_benchmark.py`, then fill in `REPORT.md` §9.2.
- Claude and Cohere on Bedrock: blocked by the account's payment method (AWS Marketplace). Also untested: the EC2 instance role's IAM policy.
- `infra/ec2/setup.sh` on a real Ubuntu 24.04 instance, and Caddy obtaining a certificate.

## Plain-UI restyle

Done (2026-11-20, polished 2026-11-23): white background, one blue accent, system font, smaller
controls, no pills, wrapping tabs, no horizontal overflow at 375 px or 1280 px.
