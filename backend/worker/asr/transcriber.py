"""Speech-to-text adapter. faster-whisper (CTranslate2) with Silero VAD and word timestamps.

The model size/device/compute type come from settings (D11: CPU int8 by default); pick them
from the WER-vs-speed benchmark in eval/, not by assumption.
"""

from __future__ import annotations

from typing import Any, Protocol

import numpy as np

from worker.asr.language import in_supported_script
from worker.asr.local_agreement import Word, normalize

# Whisper invents text for near-silent fragments ("You", "I'll see you in the next video").
# Measured on the base model: such segments had avg_logprob -1.1 to -1.3, no_speech_prob ~0.5 and
# mean word probability <= 0.47, while real speech scored -0.2 to -0.5, <= 0.07 and >= 0.8.
# Dropping only when all three agree keeps low-confidence real speech (e.g. Telugu, Hindi).
HALLUCINATION_MAX_LOGPROB = -1.0
HALLUCINATION_MIN_NO_SPEECH = 0.3
HALLUCINATION_MAX_WORD_PROB = 0.5


def is_hallucination(avg_logprob: float, no_speech_prob: float, word_probs: list[float]) -> bool:
    """True for a segment that is unsure on every signal at once: likely invented, not heard."""
    if not word_probs:
        return False
    return (
        avg_logprob < HALLUCINATION_MAX_LOGPROB
        and no_speech_prob > HALLUCINATION_MIN_NO_SPEECH
        and sum(word_probs) / len(word_probs) < HALLUCINATION_MAX_WORD_PROB
    )


# faster-whisper re-decodes at rising temperature when a greedy pass fails its checks (log-prob
# below -1, or a repetition loop). That rescues loops such as "the simplest algorithm is the
# simplest algorithm is ...", so it stays on. But if the attempt it finally returns still scores
# below -1, every attempt failed: such samples invented words in testing ("wait for the next
# one", "Henry", "邸rup 邶After"), so they are dropped.
FALLBACK_MIN_LOGPROB = -1.0


def is_failed_fallback(temperature: float, avg_logprob: float) -> bool:
    """True for a sampled (temperature > 0) result that Whisper itself still rated unreliable."""
    return temperature > 0 and avg_logprob < FALLBACK_MIN_LOGPROB


LOOP_MAX_NGRAM = 6
LOOP_MIN_REPEATS = 3


def collapse_loops(words: list[Word]) -> list[Word]:
    """Keeps one copy of a 1-6 word phrase repeated 3+ times in a row (a decoding loop)."""
    keys = [normalize(w.text) for w in words]
    out: list[Word] = []
    i = 0
    while i < len(words):
        for n in range(1, LOOP_MAX_NGRAM + 1):
            repeats = 1
            while keys[i : i + n] == keys[i + repeats * n : i + (repeats + 1) * n]:
                repeats += 1
            if repeats >= LOOP_MIN_REPEATS:
                out.extend(words[i : i + n])
                i += repeats * n
                break
        else:
            out.append(words[i])
            i += 1
    return out


class Transcriber(Protocol):
    name: str

    def transcribe(
        self, audio: np.ndarray, *, language: str | None, prompt: str | None, beam_size: int
    ) -> list[Word]:
        """Words with times relative to the start of `audio` (16 kHz float32 mono)."""
        ...


class FasterWhisperTranscriber:
    def __init__(self, model: str, device: str, compute_type: str) -> None:
        from faster_whisper import WhisperModel

        self.name = f"faster-whisper/{model}/{device}/{compute_type}"
        self._model: Any = WhisperModel(model, device=device, compute_type=compute_type)

    def transcribe(
        self, audio: np.ndarray, *, language: str | None, prompt: str | None, beam_size: int
    ) -> list[Word]:
        segments, _info = self._model.transcribe(
            audio,
            language=language,
            initial_prompt=prompt,
            beam_size=beam_size,
            word_timestamps=True,
            vad_filter=True,
            vad_parameters={"min_silence_duration_ms": 500},
            condition_on_previous_text=False,
        )
        words: list[Word] = []
        for segment in segments:
            seg_words = segment.words or []
            if is_failed_fallback(float(segment.temperature), float(segment.avg_logprob)):
                continue
            if is_hallucination(
                float(segment.avg_logprob),
                float(segment.no_speech_prob),
                [float(w.probability) for w in seg_words],
            ):
                continue
            for w in seg_words:
                if not in_supported_script(w.word):
                    continue
                words.append(
                    Word(
                        text=w.word, start=float(w.start), end=float(w.end), probability=float(w.probability)
                    )
                )
        return collapse_loops(words)
