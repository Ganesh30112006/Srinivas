"""Streaming commit policy for re-decoded windows ("local agreement", brief §4.2).

Every ~1.5 s the ASR worker re-decodes all uncommitted audio. Words that come out identical in
two consecutive runs are committed; the rest stay a partial. Committed words are grouped into
segments that close at sentence punctuation, a pause, or a maximum length. The next window starts
at the end of the last committed word (the cut), so each word is finalised exactly once.

Seams. Whisper's word end times are often a little early, so the cut can split the last committed
word; the worker therefore re-decodes ~1 s of committed audio before the cut, and the word at the
cut is heard again. All measured on real recordings, and only where the cut is the end of a
committed word (after silence moved the cut there is nothing to repeat):
- a word straddling the cut that sounds like the last committed word is that word re-heard and is
  dropped ("throughput" + "put", "wait" + "weight"); one that does not is new speech whose start
  Whisper smeared back across the pause ("CPU." + "We measure": "We" must stay);
- a hypothesis that opens with the same 1-5 words the committed text ended with drops them
  ("... quantum, for" + "For example", as in whisper_streaming);
- a short, unsure first word right at the cut is a leaked tail ("too many" + "Any context").

Segment identity: the visible partial has an id; the first segment that closes takes that id
(so the UI replaces the partial in place), later ones get fresh ids. Pure logic, no Whisper here.
"""

from __future__ import annotations

import unicodedata
import uuid
from dataclasses import dataclass, field
from difflib import SequenceMatcher

SENTENCE_END = (".", "?", "!", "।", "॥")
SEAM_MAX_WORDS = 5  # longest repeated n-gram removed at a window cut
SEAM_WINDOW_S = 1.0  # only a hypothesis starting this close to the cut can be a repeat
# Leaked fragments seen: 0.08-0.10 s long, probability 0.04-0.28, starting exactly at the cut.
FRAGMENT_START_S = 0.15
FRAGMENT_MAX_S = 0.2
FRAGMENT_MAX_PROB = 0.5
# "wait" re-heard as "weight" scores 0.6; unrelated words ("CPU" / "we") score near 0.
REHEARD_MIN_SIMILARITY = 0.5


@dataclass(frozen=True, slots=True)
class Word:
    text: str  # as produced by Whisper, usually with a leading space
    start: float  # seconds from lecture start
    end: float
    probability: float


@dataclass(frozen=True, slots=True)
class Segment:
    segment_id: str
    start_ms: int
    end_ms: int
    text: str
    confidence: float | None


@dataclass(slots=True)
class Update:
    finals: list[Segment] = field(default_factory=list)
    partial: Segment | None = None
    committed_until: float = 0.0


def normalize(text: str) -> str:
    """Case- and punctuation-insensitive form (keeps Indic vowel signs, unlike \\w)."""
    return "".join(
        c for c in text.lower() if not unicodedata.category(c).startswith(("P", "Z")) and not c.isspace()
    )


def _sounds_like(word: str, committed: str) -> bool:
    """Whether a word straddling the cut is the last committed word heard again (or its tail)."""
    return word in committed or SequenceMatcher(None, word, committed).ratio() >= REHEARD_MIN_SIMILARITY


def join_words(words: list[Word]) -> str:
    return "".join(w.text for w in words).strip()


def _segment(segment_id: str, words: list[Word]) -> Segment:
    return Segment(
        segment_id=segment_id,
        start_ms=round(words[0].start * 1000),
        end_ms=round(words[-1].end * 1000),
        text=join_words(words),
        confidence=round(sum(w.probability for w in words) / len(words), 3),
    )


class LiveCommitter:
    def __init__(
        self,
        id_namespace: uuid.UUID,
        committed_until: float = 0.0,
        *,
        pause_s: float = 0.6,
        max_segment_s: float = 12.0,
        max_window_s: float = 25.0,
    ) -> None:
        self.namespace = id_namespace
        self.committed_until = committed_until
        self.pause_s = pause_s
        self.max_segment_s = max_segment_s
        self.max_window_s = max_window_s
        self.previous: list[Word] = []  # last hypothesis beyond the committed point
        self.pending: list[Word] = []  # committed words not yet closed into a segment
        self.tail: list[Word] = []  # last committed words, to spot a repeat at the next cut
        self.open_id = self._new_id()

    def _new_id(self) -> str:
        # Deterministic per lecture + position: a restarted worker regenerates the same ids.
        return str(uuid.uuid5(self.namespace, f"{self.committed_until:.3f}:{len(self.pending)}"))

    def _uncommitted(self, hypothesis: list[Word]) -> list[Word]:
        cut = self.committed_until + 1e-3
        # A seam exists only where the cut is the end of the last committed word. After silence
        # moved the cut forward nothing was committed there, so every word ending after the cut
        # is new speech (seen twice: "We measure" lost its "We" to a seam rule).
        at_word_end = bool(self.tail) and self.committed_until - self.tail[-1].end < 1e-3
        if not at_word_end:
            return [w for w in hypothesis if w.end > cut and normalize(w.text)]
        last = normalize(self.tail[-1].text)
        words = [
            w
            for w in hypothesis
            if w.end > cut
            and normalize(w.text)
            and not ((w.start + w.end) / 2 <= cut and _sounds_like(normalize(w.text), last))
        ]
        if not words or words[0].start - self.committed_until > SEAM_WINDOW_S:
            return words
        head = words[0]
        if (
            head.start - self.committed_until < FRAGMENT_START_S
            and head.end - head.start < FRAGMENT_MAX_S
            and head.probability < FRAGMENT_MAX_PROB
        ):
            return words[1:]
        for n in range(1, min(SEAM_MAX_WORDS, len(self.tail), len(words)) + 1):
            if [normalize(w.text) for w in self.tail[-n:]] == [normalize(w.text) for w in words[:n]]:
                return words[n:]
        return words

    def _commit(self, commit: list[Word]) -> None:
        self.pending.extend(commit)
        self.tail = (self.tail + commit)[-SEAM_MAX_WORDS:]

    def update(self, hypothesis: list[Word], window_end: float) -> Update:
        words = self._uncommitted(hypothesis)
        agreed = 0
        while (
            agreed < len(words)
            and agreed < len(self.previous)
            and normalize(words[agreed].text) == normalize(self.previous[agreed].text)
        ):
            agreed += 1
        commit = words[:agreed]
        # Safety valve: never let the window grow without bound (e.g. unstable noisy audio).
        if not commit and window_end - self.committed_until > self.max_window_s:
            commit = [w for w in words if w.end <= window_end - 5.0] or words[:1]
        self.previous = words[len(commit) :]
        if commit:
            self._commit(commit)
            self.committed_until = commit[-1].end
        elif not words and not self.pending:
            # Silence (VAD removed everything): move the window forward, keeping 1 s for a word
            # that may be starting right at the edge.
            self.committed_until = max(self.committed_until, window_end - 1.0)
        finals = self._close_segments(final=False)
        return Update(finals=finals, partial=self._partial(), committed_until=self.committed_until)

    def force(self, hypothesis: list[Word], until: float) -> Update:
        """Catch-up mode (worker behind live): commits words ending before `until` without
        waiting for agreement, because the audio is already complete there."""
        words = self._uncommitted(hypothesis)
        commit = [w for w in words if w.end <= until]
        self.previous = []
        if commit:
            self._commit(commit)
        self.committed_until = max(self.committed_until, commit[-1].end if commit else until)
        finals = self._close_segments(final=False)
        return Update(finals=finals, partial=self._partial(), committed_until=self.committed_until)

    def flush(self, until: float | None = None) -> list[Segment]:
        """Commits everything (end of lecture, or audio stops at a gap)."""
        self._commit(self.previous)
        self.previous = []
        if self.pending:
            self.committed_until = max(self.committed_until, self.pending[-1].end)
        if until is not None and until > self.committed_until + 1e-3:
            # Jumping over audio that is not this committer's (a gap, or the end of a run): there
            # is no seam to repair on the far side, and no context to re-decode.
            self.committed_until = until
            self.tail = []
        return self._close_segments(final=True)

    def _partial(self) -> Segment | None:
        words = self.pending + self.previous
        if not words:
            return None
        return Segment(
            self.open_id, round(words[0].start * 1000), round(words[-1].end * 1000), join_words(words), None
        )

    def _close_segments(self, *, final: bool) -> list[Segment]:
        finals: list[Segment] = []
        start = 0
        for i, word in enumerate(self.pending):
            next_word = self.pending[i + 1] if i + 1 < len(self.pending) else None
            sentence_end = word.text.rstrip().endswith(SENTENCE_END)
            pause = next_word is not None and next_word.start - word.end >= self.pause_s
            too_long = word.end - self.pending[start].start >= self.max_segment_s
            if sentence_end or pause or too_long:
                finals.append(_segment(self.open_id, self.pending[start : i + 1]))
                start = i + 1
                self.open_id = str(uuid.uuid5(self.namespace, f"{word.end:.3f}:seg"))
        self.pending = self.pending[start:]
        if final and self.pending:
            finals.append(_segment(self.open_id, self.pending))
            self.open_id = str(uuid.uuid5(self.namespace, f"{self.pending[-1].end:.3f}:seg"))
            self.pending = []
        return finals
