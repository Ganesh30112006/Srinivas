"""FasterWhisperTranscriber retries with auto language detection when the chosen one finds nothing."""

from __future__ import annotations

import numpy as np

from worker.asr.local_agreement import Word
from worker.asr.transcriber import FasterWhisperTranscriber


def _fake(results: dict[str | None, list[Word]]) -> tuple[FasterWhisperTranscriber, list[str | None]]:
    t = FasterWhisperTranscriber.__new__(FasterWhisperTranscriber)  # no model download
    calls: list[str | None] = []

    def run(audio: np.ndarray, *, language: str | None, prompt: str | None, beam_size: int) -> list[Word]:
        calls.append(language)
        return results.get(language, [])

    t._transcribe = run  # type: ignore[method-assign]
    return t, calls


def test_wrong_language_falls_back_to_detection() -> None:
    hello = [Word(" hello", 0.0, 0.4, 0.9)]
    t, calls = _fake({None: hello})
    assert t.transcribe(np.zeros(16000, dtype=np.float32), language="te", prompt=None, beam_size=1) == hello
    assert calls == ["te", None]


def test_no_retry_when_the_language_works_or_is_already_auto() -> None:
    hello = [Word(" hello", 0.0, 0.4, 0.9)]
    t, calls = _fake({"en": hello})
    t.transcribe(np.zeros(16000, dtype=np.float32), language="en", prompt=None, beam_size=1)
    t.transcribe(np.zeros(16000, dtype=np.float32), language=None, prompt=None, beam_size=1)
    assert calls == ["en", None]
