# IoT-Based Smart Classroom Lecture Capture and Real-Time AI Speech-to-Text System

**Project report: B.Tech, Computer Science and Engineering**

> Fill in before submission: student names and roll numbers, guide, department, institution, academic year, and the remaining results in Chapter 9. Chapter 9 already has results measured on synthetic speech, a local stack and the device simulator; each one says so. Every number not yet measured is marked *to be measured*.

---

## Abstract

Students in Indian classrooms often miss parts of a lecture: they fall behind while taking notes, they are absent, or the lecturer switches between English, Telugu and Hindi. This project builds a complete system that records a lecture with a small device on the lecturer's desk and shows a live, multilingual transcript in students' browsers within seconds. After the lecture it generates study material.

The device is an ESP32 microcontroller (ESP32 DevKit V1) with an INMP441 I²S MEMS microphone, an OLED display, a MicroSD card, status LEDs and one main button (start, pause, resume; hold to stop). It streams 0.5-second audio chunks over a WebSocket. If Wi-Fi drops, it keeps recording onto the MicroSD card and resends the backlog when the connection returns, so no audio is lost. The server runs on a single AWS EC2 instance without containers:
- a device gateway, a REST/WebSocket API, a speech-recognition worker and an AI worker, all in Python with FastAPI;
- PostgreSQL with pgvector, and Redis.

Speech recognition uses faster-whisper (Whisper large-v3 class models) with a local-agreement streaming algorithm. Study material comes from Amazon Bedrock: notes, a summary, key terms, practice questions, flashcards and revision notes, plus a question-answering feature grounded in the transcript. The web application is built with React and TypeScript. It provides:
- a live lecture view with device monitoring and recording controls;
- a lecture library, search, and study tabs;
- device and user administration.

The software is covered by automated tests: backend, evaluation tooling, frontend unit tests, and end-to-end browser tests including accessibility checks. Evaluation scripts measure word error rate, end-to-end latency and recovery after network outages.

**Keywords:** IoT, ESP32, I²S microphone, speech-to-text, Whisper, code-switching, WebSocket, Amazon Bedrock, retrieval-augmented generation, lecture capture.

---

## Table of contents

1. Introduction
2. Literature survey
3. Requirements
4. System architecture
5. Hardware design
6. Firmware design
7. Server design
8. Web application
9. Testing and results
10. Deployment
11. Limitations and future work
12. Conclusion
- References
- Appendices

---

## 1. Introduction

### 1.1 Background

Lectures are the main channel of teaching in engineering colleges. They are spoken, fast and usually not recorded. Students who take notes by hand lose part of what is said while they write. Students with hearing difficulty, students who were absent, and students who are not fluent in the language being spoken lose more. In many Indian classrooms a lecturer explains a concept in English, repeats it in Telugu or Hindi, and mixes languages within a sentence (code-switching), which makes manual note taking harder still.

### 1.2 Problem statement

Design and build a low-cost classroom system that:
1. captures the lecturer's speech reliably, even when the classroom Wi-Fi is unreliable;
2. shows a live transcript in English, Telugu and Hindi with a delay of a few seconds;
3. turns the finished transcript into study material that students can search and revise from;
4. respects consent and data-retention obligations for recorded audio.

### 1.3 Objectives

| # | Objective | Where it is met |
|---|---|---|
| O1 | Capture 16 kHz speech with a dedicated desk device | Chapter 5, 6 |
| O2 | No audio loss during network outages | §6.4, §9.4 |
| O3 | Live multilingual transcript within seconds | §7.3, §9.3 |
| O4 | AI-generated notes, summary, key terms, questions, flashcards, revision notes | §7.4 |
| O5 | Search across lectures and question answering grounded in the transcript | §7.4, §8 |
| O6 | Role-based web application for students, lecturers and administrators | Chapter 8 |
| O7 | Deployable on one cloud server without containers | Chapter 10 |
| O8 | Measurable accuracy, latency and reliability | Chapter 9 |

### 1.4 Scope

**In scope:**
- one device per classroom;
- English, Telugu and Hindi;
- a single-server deployment for one college.

**Out of scope:**
- video capture;
- speaker diarisation (telling apart several speakers);
- graded examinations;
- mobile apps (the web app works on phones).

---

## 2. Literature survey

> Replace this chapter's summaries with the papers your guide asks you to cite, and add full references.

| Area | Existing approach | Gap this project addresses |
|---|---|---|
| Lecture capture systems | Room cameras and microphones that record to a server for later viewing | Recordings are watched after class, not read live, and are rarely searchable |
| Cloud speech APIs | Streaming ASR services from large cloud providers | Cost per minute; weak support for Telugu–English code-switching; audio leaves the institution |
| Whisper | Open, multilingual encoder–decoder ASR trained on large weakly supervised data | Not designed for streaming; it needs a policy for when partial text is "final" |
| Streaming Whisper (local agreement) | Re-decode a sliding window and commit only the prefix that two consecutive passes agree on | Used here with a fixed decode language for stability (decision D6) |
| Embedded audio on ESP32 | I²S MEMS microphones with Wi-Fi streaming | Few designs handle offline buffering and exactly-once delivery |
| Retrieval-augmented generation | Answer questions from retrieved passages, with citations | Applied here to a single lecture transcript with timestamps |

---

## 3. Requirements

### 3.1 Functional requirements

| ID | Requirement |
|---|---|
| F1 | The lecturer prepares a lecture (subject, title, language) and confirms that students have been told it is recorded. |
| F2 | Recording starts from the device's Start button or from the web Live page, and can be paused and resumed from either. |
| F3 | Students see the transcript live, with partial lines that settle into final lines. |
| F4 | If the network drops, the device keeps recording; the transcript shows the gap and fills it when the audio arrives. |
| F5 | After Stop, the system produces notes, a summary, key terms, practice questions, flashcards and revision notes. |
| F6 | The lecturer can correct transcript lines, edit the notes, and publish them to students. |
| F7 | Users can search all lectures they can see and jump to the exact moment in a transcript. |
| F8 | Students can ask questions about a lecture and get answers that cite transcript moments. |
| F9 | Administrators manage subjects, users and devices: register, assign to lecturers, set microphone gain and voice detection. |
| F10 | Transcripts, notes and questions can be exported. |
| F11 | Recorded audio can be deleted automatically after a configured number of days. |

### 3.2 Non-functional requirements

| ID | Requirement | Target |
|---|---|---|
| N1 | Live transcript latency (speech → screen) | a few seconds; locally 9.3 s p50 with `small` on a laptop CPU, limited by decoding (§9.3); *to be measured* on the server |
| N2 | Audio loss during a Wi-Fi outage | zero chunks lost, zero duplicates, within the buffer capacity (§6.4) |
| N3 | Offline buffer | about 20 minutes of audio on the MicroSD card (4.4 minutes in internal flash without a card) |
| N4 | Security | HTTPS/WSS only; per-device tokens stored as hashes; server-side sessions with CSRF protection |
| N5 | Accessibility | WCAG 2.1 AA, checked automatically with axe on every page |
| N6 | Frontend size | initial JavaScript ≤ 200 KB gzip |
| N7 | Privacy | consent before students can see a lecture; configurable audio retention (India's DPDP Act 2023) |

### 3.3 Hardware and software used

| Hardware | Software |
|---|---|
| ESP32 DevKit V1 (ESP32-WROOM-32, 4 MB flash, no PSRAM) with a MicroSD card | ESP-IDF 5.5 (C) |
| INMP441 I²S MEMS microphone | Python 3.12, FastAPI, SQLAlchemy, Alembic |
| SSD1306 0.96" OLED (I²C) | PostgreSQL 16 + pgvector, Redis |
| Common-cathode RGB LED, 3 push buttons | faster-whisper (CTranslate2) |
| 5 V USB power supply | Amazon Bedrock (LLM + embeddings) |
| AWS EC2 instance (Ubuntu 24.04) | React 19, TypeScript, Vite, TanStack Query |
| | Caddy (TLS), systemd |

---

## 4. System architecture

```mermaid
flowchart LR
  subgraph Classroom
    MIC[INMP441 mic] -->|I2S 16 kHz| ESP[ESP32 recorder<br/>RAM ring + SD card spill]
  end
  ESP -->|wss /ws/device| GW[Device gateway]
  subgraph Server[EC2, Ubuntu 24.04, systemd]
    CADDY[Caddy TLS] --> GW
    CADDY --> API[REST + live WebSocket API]
    CADDY --> WEB[Static web app]
    GW --> PG[(PostgreSQL + pgvector)]
    GW --> R[(Redis)]
    ASR[ASR worker] --> PG
    ASR --> R
    NOTES[Notes worker] --> PG
    API --> PG
    API --> R
  end
  NOTES --> BR[Amazon Bedrock]
  API --> BR
  BROWSER[Browser] --> CADDY
```

**Data flow:**
1. The device sends 0.5 s audio chunks, each numbered with a sequence number.
2. The gateway stores each chunk exactly once, keyed on `(lecture_id, seq)`. It acknowledges the chunk and queues the lecture for speech recognition.
3. The ASR worker decodes sliding windows of audio and publishes partial and final transcript events to the lecture's Redis Stream.
4. The API relays the stream to every browser watching the lecture. A browser that reconnects resumes from the last event it saw.
5. After Stop, the ASR worker finalises the transcript and the notes worker generates the study material.

PostgreSQL is the source of truth; Redis holds only short-lived queues and live streams. The main design decisions (D1–D14) are recorded in [docs/DECISIONS.md](../DECISIONS.md). Diagrams are in [UML.md](UML.md).

---

## 5. Hardware design

The circuit and wiring are in [CIRCUIT.md](CIRCUIT.md). Summary:

| Part | Connection |
|---|---|
| INMP441 | SCK→D26, WS→D25, SD→D33 (100 kΩ pull-down), L/R→GND, VDD→3V3 with a 100 nF decoupling capacitor |
| SSD1306 OLED | SDA→D21, SCL→D22, address 0x3C |
| MicroSD adapter | CS→D5, SCK→D18, MISO→D19, MOSI→D23, VCC→VIN (5 V) |
| Buttons | Main→D32 (the board's BOOT button does the same), optional Stop→D27, each to GND with the internal pull-up |
| Status LEDs | red/green/third → GPIO16/17/4 through 100–220 Ω; short legs to GND |

**Why these parts:**
- The ESP32 has two cores, hardware I²S and Wi-Fi. It has no PSRAM, so only 5 s of audio stays in RAM (as ADPCM) and everything older moves to the SD card.
- The design started on an ESP32-S3 with 8 MB of PSRAM; the firmware still builds for it (pins in `firmware/board/board.h`).
- The INMP441 produces a digital signal, so there are no analog noise problems on the board.
- The OLED and LED show the lecturer the recording state at a glance without opening the website.

**Offline storage:**
- Offline audio goes to a preallocated 20-minute log file on the MicroSD card, in the same crash-safe record format as the flash log (CRC per record, ACK marks written in place).
- Without a card the device falls back to internal flash, about 4.4 minutes.
- An earlier plan used PSRAM plus internal flash and no card (about 12.7 minutes on the ESP32-S3). The board actually used has no PSRAM and only 4 MB of flash, so the MicroSD card became necessary.
- Over jumper wires the card needed pull-ups on its SPI lines and a 10 MHz clock; at that setting it writes 470 KB/s, about 50 times what the recorder needs.

---

## 6. Firmware design

### 6.1 Tasks

| Task | Core | Job |
|---|---|---|
| `audio` | 1 | I²S DMA capture, 24→16-bit conversion, high-pass filter, gain, 500 ms chunks (ADPCM) into the RAM ring, level meter, silence warning |
| `net` | 0 | Wi-Fi with back-off, TLS WebSocket, `hello`/`config`, live-first sending with selective ACKs, web commands, status reports |
| `spill` | 0 | Moves un-acknowledged chunks to the flash log as IMA-ADPCM when offline |
| `ui` | 0 | LED and OLED |
| `buttons` | 0 | Start / Stop / Pause with debouncing |

### 6.2 States

`BOOT → WIFI_CONNECTING → SERVER_CONNECTING → READY → RECORDING ⇄ PAUSED`. `RECORDING` becomes `RECORDING_OFFLINE` when the server is unreachable. Stop leads through `SYNCING` and `FINALIZING` back to `READY`. The full diagram is in [UML.md](UML.md).

### 6.3 Protocol

Each audio frame has a 26-byte header: magic `LC`, version, codec (PCM16 or IMA-ADPCM), sequence number, sample offset, capture time, and payload length. JSON control messages carry `hello`, `start`, `resume`, `pause`, `stop`, `status` and `ack`. The full specification is in [contracts/PROTOCOL.md](../../contracts/PROTOCOL.md).

### 6.4 No audio loss: how it works

1. Every chunk stays in the RAM ring or the SD log until the server acknowledges it.
2. While the server is unreachable, any chunk older than 2 s is written to the log on the SD card (already IMA-ADPCM, 4:1). A power cut therefore loses at most about 2 s.
3. On reconnect, the device sends **live audio first** and the backlog in spare bandwidth. This keeps the live transcript current.
4. The server acknowledges with a cumulative sequence number plus "ranges" of stored chunks above it (selective ACKs). The device frees exactly what was stored.
5. The server ignores a chunk it already has, so resending is safe (idempotent storage).
6. Log records carry a CRC and the session id, so a recording interrupted by a reboot is recovered and uploaded.

---

## 7. Server design

### 7.1 Components

| Process | Responsibility |
|---|---|
| Device gateway | Authenticates devices by token hash; stores chunks; detects gaps; relays web commands (start, pause, resume, stop) to the device; records status (signal, buffers, mic level) |
| API | REST endpoints, cookie sessions with CSRF, role checks, the per-lecture live WebSocket |
| ASR worker | Streaming recognition with faster-whisper and local agreement; per-segment language detection; finalises after Stop; fills gaps (backfill) |
| Notes worker | Calls Amazon Bedrock to produce notes, summary, key terms, questions, flashcards and revision notes; computes embeddings for search and Ask |
| Retention tool | Deletes audio older than `AUDIO_RETENTION_DAYS`, keeping transcripts and notes |

### 7.2 Data model

The main tables:
- users, sessions, subjects, devices, device_lecturers, lectures;
- audio_chunks, lecture_gaps, transcript_segments, lecture_artifacts;
- qa_messages, audit_log, latency_marks, asr_progress.

The class diagram is in [UML.md](UML.md).

### 7.3 Streaming speech recognition

- Whisper processes 30-second windows and is not a streaming model.
- The worker re-decodes a sliding window every 1.5 s (`ASR_LIVE_INTERVAL_S`). It commits only the words that two consecutive decodes agree on (*local agreement*).
- Uncommitted text is sent as a *partial*, and committed text as a *final*.
- **Window seams:** Whisper's word end times are often slightly early, so a cut at the last committed word can split it. Each decode therefore re-hears 1 s of already-committed audio. A word counts as new only if most of it lies after the cut. A 1–5 word repeat of the committed text at the cut is dropped, as is a very short, low-confidence fragment there. In testing with real faster-whisper, this removed doubled words ("for For example") and stray fragments ("too many Any"), and it recovered words that had been mangled at the cut ("POOT" → "throughput").
- **No text prompt:** Whisper can be prompted with the previous transcript text. In testing, this made short windows repeat the prompt or degenerate into junk, and the window's real speech was lost. Decodes therefore use the 1 s of context audio instead of a prompt.
- **Unreliable decodes:** as backstops, a temperature-fallback result that still scores below −1 log-probability is dropped, a phrase repeated three or more times in a row is collapsed, and words in scripts a lecture cannot contain (anything other than Latin, Telugu or Devanagari) are dropped.
- **Hallucinations:** a segment that is uncertain on every signal is discarded: average log-probability below −1, no-speech probability above 0.3, and mean word probability below 0.5. Whisper produces such segments on near-silent fragments ("You", "I'll see you in the next video").
- The decode language is fixed to the lecture's language hint, so a language switch cannot stall agreement (D6). The language of each final segment is detected afterwards and shown as EN/TE/HI.

### 7.4 AI features

- **Study material:** Amazon Bedrock generates notes, a summary, key terms, practice questions, flashcards and revision notes from the final transcript. Each prompt asks for structured JSON, and the output is validated before it is stored.
- **Live key terms:** a light pass refreshes key terms every few minutes during recording (D4).
- **Ask:** answers are generated only from the lecture's transcript segments and return the ids of the segments they cite. The web app turns these into "Jump to mm:ss" links.
- **Visibility:** students see AI material only after the lecturer publishes it.

---

## 8. Web application

| Page | Who | What it does |
|---|---|---|
| Home | all | Upcoming and recent lectures; lecturers prepare a new lecture |
| Live | all | Live transcript with partial lines, gap markers and backfill. Lecturers also get Start/Pause/Resume/Stop and line correction. Device monitor: mic level, mic status, Wi-Fi, buffers, sync progress |
| Library | all | Lectures by subject, date and status |
| Lecture | all | Tabs: Transcript, Notes, Summary, Revision, Key terms, Flashcards, Questions, Ask; export |
| Search | all | Full-text search across lectures, with highlighted matches that open the transcript at that moment |
| Devices | lecturer, admin | Device health. Admins register devices, assign lecturers, and set gain and voice detection |
| Settings | all | Profile and password; theme, transcript text size, languages, notifications. Admins also see the server configuration and retention policy |
| Admin | admin | Subjects and users |

The REST types are generated from the server's OpenAPI document, so the client and server cannot silently disagree.

---

## 9. Testing and results

### 9.1 Automated tests

| Part | Tool | Scope |
|---|---|---|
| Backend | pytest on an embedded PostgreSQL | Device protocol over real WebSockets, outage and recovery with a device simulator, ASR pipeline with a deterministic fake model, notes with a scripted LLM, API permissions, retention |
| Evaluation | pytest | WER/CER with Indic-safe normalisation, percentiles, latency pairing, recovery checks |
| Frontend | Vitest + Testing Library + MSW | Pages and components against a mock API and a fake live socket |
| End-to-end | Playwright + axe | Prepare → live, outage gap → backfill, reconnect without duplicates, search → timestamp, quiz, export; WCAG 2.1 A/AA on every page for every role |
| Firmware | host unit tests (CMake or zig cc); `firmware/tools/compile_check.py` | Framing, ADPCM, ACK ranges, ring buffer, flash log, back-off (80 checks, also under UBSan); every firmware file compiled for the ESP32-S3 with `-Werror`, both apps linked against ESP-IDF 5.5, recorder image 1.05 MB of 3 MB. Not yet run on the board |

Static checks: ruff and `mypy --strict` (Python); `tsc`, ESLint and Prettier (TypeScript); a CSS design-token lint; a bundle-size budget.

### 9.2 Transcription accuracy

Measure on real classroom recordings with `eval/asr_benchmark.py` (see `eval/README.md`). Report WER for English and CER for Telugu and Hindi.

**Measured so far (2026-11-24): synthetic speech only.** These are not classroom results. The test set is four lecture texts (`eval/synthetic/`), each spoken by four Windows voices (US English: Zira, David; Indian English: Heera, Ravi): 16 recordings, 21.5 minutes, 3,156 reference words, 16 kHz mono. The machine was a laptop with an Intel Core i7-1255U and 16 GB RAM, running faster-whisper on the CPU (int8). Synthetic speech is clean and evenly paced, so real classrooms will have higher error rates.

| Model | Decoding | Speech | WER | CER | Real-time factor |
|---|---|---|---|---|---|
| Whisper `base` | beam 5 | all 16 | 2.5% | 1.3% | 0.086 |
| Whisper `small` | beam 5 | all 16 | 1.9% | 1.0% | 0.276 |
| Whisper `base` | live (beam 1) | all 16 | 2.5% | 1.3% | 0.075 |
| Whisper `small` | live (beam 1) | all 16 | 1.8% | 1.1% | 0.209 |
| Whisper `small` | live (beam 1) | US English voices | 1.7% | 1.1% | 0.177 |
| Whisper `small` | live (beam 1) | Indian English voices | 2.0% | 1.1% | 0.185 |

Findings:
- `small` makes about a quarter fewer word errors than `base`, at about three times the compute.
- Errors are mostly technical words, for example "convoy effect" written as "convo effect" in both Indian-English voices.
- Live decoding (beam 1) is as accurate as beam 5 here.

Still *to be measured*, because they need real recordings (Windows has no Telugu or Hindi voices):

| Model | Language | WER / CER | Real-time factor |
|---|---|---|---|
| *to be measured* | English (classroom) | | |
| *to be measured* | Telugu | | |
| *to be measured* | Hindi | | |
| *to be measured* | Code-switched | | |

### 9.3 Latency

Use `eval/latency_report.py` on a recorded lecture session.

**Measured so far (2026-11-24): one local full-stack run, not a deployment.** All services ran on the laptop from §9.2: the API, the gateway, the `small` ASR worker on the CPU, the notes worker, the browser and the device simulator. The run streamed a 64 s synthetic lecture with a 10 s network outage (129 chunks). The latency report pairs chunks with decoding windows as described in `eval/README.md`.

| Stage | n | p50 | p95 |
|---|---|---|---|
| Capture → gateway (live chunks) | 109 | 0.52 s | 0.54 s |
| Gateway → decoding starts | 129 | 2.8 s | 11.3 s |
| Decoding one window (`small`, CPU) | 129 | 4.6 s | 5.3 s |
| Published → shown in the browser | 21 | 0.18 s | 0.24 s |
| **Capture → text first shown** (partial or final) | 122 | **9.3 s** | **23.8 s** |

The bottleneck is decoding. Each live window takes about 4.6 s with `small` on this laptop CPU, so chunks queue behind it. The p95 also includes the 10 s outage backlog. The network and browser stages take under a second together. Measures to bring the delay down to a few seconds:
- a faster CPU, or a GPU (`ASR_DEVICE=cuda`);
- the `base` model, which decodes about three times faster;
- a longer live interval.

Measure again on the EC2 instance.

| Measure | p50 | p95 |
|---|---|---|
| Capture → partial shown (deployment) | *to be measured* | |
| Capture → final shown (deployment) | *to be measured* | |

### 9.4 Network recovery

Use `eval/recovery_report.py` after pulling the Wi-Fi during recording.

**Measured so far (2026-11-24): the device simulator on the local stack, not the board.** The simulator speaks the device protocol exactly (`backend/tools/device_simulator.py`). It stops sending for the outage, keeps recording, and sends the backlog when it reconnects. Longer runs were sped up (the "Speed" column); outages are in lecture time.

| Outage | Lecture | Speed | Chunks sent | Chunks stored | Lost | Duplicates | Time to fill the gap after reconnecting |
|---|---|---|---|---|---|---|---|
| 10 s | 64 s | 1× | 129 | 129 | 0 | 0 | under 0.7 s |
| 30 s | 2.8 min | 1× | 340 | 340 | 0 | 0 | under 0.6 s |
| 2 min | 5.3 min | 2× | 641 | 641 | 0 | 0 | under 0.4 s |
| 10 min | 13.6 min | 4× | 1,632 | 1,632 | 0 | 0 | under 0.3 s |

How to read the results:
- In every run, each chunk was stored exactly once and the report passed.
- The longest capture-to-arrival delay in each run equals the outage's wall-clock length plus the time to fill the gap, so the backlog went out almost immediately after reconnecting.
- Live chunks outside the gaps arrived 0.15–0.5 s after capture (p50).
- The 10-minute outage fits the device's offline buffer (about 20 minutes on the SD card).

Still *to be measured* on the board, with the Wi-Fi uplink pulled:
- the flash spill and I²S timing;
- a power cut during the outage.

---

## 10. Deployment

The deployment uses one Ubuntu 24.04 EC2 instance with no Docker (D12):
- PostgreSQL, Redis and Caddy are installed as system packages;
- the four Python services run as systemd units;
- `infra/ec2/setup.sh` prepares the machine once, and `infra/ec2/deploy.sh` installs each release;
- Bedrock is accessed through the instance's IAM role.

Details are in [infra/README.md](../../infra/README.md).

---

## 11. Limitations and future work

**Limitations:**
- The firmware and the real speech and AI models were not run inside the automated test suite. They must be validated on hardware and real recordings (Chapter 9).
- The offline buffer holds about 20 minutes (RAM limits the index, not the card); a longer outage loses audio, and the device warns on screen.
- One gateway process holds the device connections, so it does not scale horizontally as written.
- Practice-question answers are sent to the browser, which is fine for revision but not for graded tests.

**Future work:**
- speaker diarisation (lecturer vs. student questions);
- on-device voice activity detection to save bandwidth;
- a longer SD log for very long outages, by indexing the card in pages instead of keeping the whole index in RAM;
- fine-tuning ASR on local classroom audio;
- a mobile app with offline notes;
- an LMS integration (e.g. Moodle).

---

## 12. Conclusion

The project delivers a working path from a classroom microphone to a live, multilingual transcript and AI study material:
- The device keeps recording through network outages and delivers every chunk exactly once.
- The server turns audio into a live transcript with a streaming policy on top of Whisper.
- Study material from Amazon Bedrock is grounded in the transcript and links back to the moment in the lecture it came from.

The system is built to be measured: the evaluation scripts report accuracy, latency and recovery, so its claims can be checked on real classroom recordings.

---

## References

> Complete these in your department's citation style.

1. A. Radford et al., "Robust Speech Recognition via Large-Scale Weak Supervision" (Whisper), 2022.
2. Espressif Systems, *ESP32 Technical Reference Manual* and *ESP-IDF Programming Guide*.
3. InvenSense (TDK), *INMP441 Omnidirectional Microphone with I²S Digital Output*, datasheet.
4. Solomon Systech, *SSD1306 OLED driver*, datasheet.
5. Amazon Web Services, *Amazon Bedrock User Guide* (Converse API, embeddings).
6. IETF RFC 6455, *The WebSocket Protocol*.
7. W3C, *Web Content Accessibility Guidelines (WCAG) 2.1*.
8. Government of India, *Digital Personal Data Protection Act, 2023*.

## Appendices

- A. Wiring and circuit: [CIRCUIT.md](CIRCUIT.md)
- B. UML diagrams: [UML.md](UML.md)
- C. User manual: [USER_MANUAL.md](USER_MANUAL.md)
- D. Device and browser protocol: [contracts/PROTOCOL.md](../../contracts/PROTOCOL.md)
- E. Demo script: [DEMO_SCRIPT.md](DEMO_SCRIPT.md)
