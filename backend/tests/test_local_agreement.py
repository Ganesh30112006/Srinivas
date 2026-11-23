import uuid

from worker.asr.language import in_supported_script, script_language, transliterate
from worker.asr.local_agreement import LiveCommitter, Word, normalize
from worker.asr.transcriber import collapse_loops, is_failed_fallback, is_hallucination

NS = uuid.UUID("12345678-1234-5678-1234-567812345678")


def words(*items: tuple[str, float, float]) -> list[Word]:
    return [Word(text, start, end, 0.9) for text, start, end in items]


def test_commits_only_words_agreed_by_two_consecutive_runs() -> None:
    c = LiveCommitter(NS)
    first = c.update(words((" Round", 0.0, 0.4), (" robin", 0.4, 0.8)), window_end=1.0)
    assert first.finals == [] and first.partial is not None and first.partial.text == "Round robin"
    assert c.committed_until == 0.0
    second = c.update(words((" Round", 0.0, 0.4), (" robin", 0.4, 0.8), (" uses", 0.8, 1.1)), window_end=1.5)
    assert c.committed_until == 0.8  # "Round robin" agreed → committed
    assert second.finals == []  # no sentence end yet: still part of the open segment
    assert second.partial is not None and second.partial.text == "Round robin uses"


def test_sentence_end_closes_segment_and_partial_keeps_its_id() -> None:
    c = LiveCommitter(NS)
    hyp = words((" Time", 0.0, 0.3), (" quantum.", 0.3, 0.9), (" Next", 1.2, 1.5))
    partial = c.update(hyp, 1.6).partial
    assert partial is not None
    update = c.update(hyp + words((" topic", 1.5, 1.9)), 2.0)
    assert [s.text for s in update.finals] == ["Time quantum."]
    assert update.finals[0].segment_id == partial.segment_id  # final replaces the partial in place
    assert update.finals[0].start_ms == 0 and update.finals[0].end_ms == 900
    assert update.partial is not None and update.partial.segment_id != partial.segment_id
    assert update.partial.text == "Next topic"


def test_disagreement_is_not_committed() -> None:
    c = LiveCommitter(NS)
    c.update(words((" The", 0.0, 0.2), (" convoy", 0.2, 0.6)), 1.0)
    c.update(words((" The", 0.0, 0.2), (" convey", 0.2, 0.6)), 1.5)
    assert c.committed_until == 0.2  # only "The" agreed
    c.update(words((" convoy", 0.2, 0.6), (" effect", 0.6, 1.0)), 2.0)
    assert c.committed_until == 0.2  # "convoy" vs previous "convey": still unstable


def test_pause_and_max_length_close_segments() -> None:
    c = LiveCommitter(NS, max_segment_s=1.5)
    hyp = words(
        (" one", 0.0, 0.5), (" two", 1.5, 2.0), (" three", 2.1, 2.6), (" four", 2.7, 3.2), (" five", 3.3, 3.6)
    )
    c.update(hyp, 4.0)
    update = c.update(hyp, 4.5)
    assert [s.text for s in update.finals] == ["one", "two three four"]


def test_flush_commits_everything_once() -> None:
    c = LiveCommitter(NS)
    hyp = words((" final", 0.0, 0.4), (" words", 0.4, 0.8))
    c.update(hyp, 1.0)
    finals = c.flush()
    assert [s.text for s in finals] == ["final words"]
    assert c.flush() == []
    # Words before the committed point are never emitted again.
    assert c.update(hyp, 1.5).partial is None


def test_window_safety_valve_forces_progress() -> None:
    c = LiveCommitter(NS, max_window_s=10.0)
    c.update(words((" a", 0.0, 1.0)), 5.0)
    c.update(words((" b", 0.0, 1.0)), 11.0)  # never agrees, window too long
    assert c.committed_until > 0.0


def test_normalize_keeps_indic_vowel_signs() -> None:
    assert normalize(" షెడ్యూలింగ్,") == "షెడ్యూలింగ్"
    assert normalize("Quantum!") == normalize("quantum")


def test_script_language_and_transliteration() -> None:
    assert script_language("Round robin uses a time quantum.") == "en"
    assert script_language("ఇప్పుడు మనం CPU scheduling గురించి చూద్దాం.") == "te"
    assert script_language("Priority scheduling में starvation की problem आती है।") == "hi"
    assert script_language("123 ...") == "en"
    assert script_language("Okay, अच्छा let us continue with round robin now") == "en"
    assert transliterate("ఇప్పుడు మనం CPU scheduling", "te") == "ippuḍu manaṁ CPU scheduling"
    assert transliterate("शेड्यूलिंग है", "hi") == "śēḍyūliṁga hai"
    assert transliterate("hello", "en") is None


def test_word_reheard_at_the_window_cut_is_not_committed_twice() -> None:
    # Seen with real faster-whisper: "... a fixed time quantum, for For example 4 milliseconds."
    c = LiveCommitter(NS)
    hyp = words((" quantum,", 0.0, 0.5), (" for", 0.5, 0.8))
    c.update(hyp, 1.0)
    c.update(hyp, 1.5)
    assert c.committed_until == 0.8
    # The window now starts at 0.8 and Whisper hears the tail of "for" again.
    again = words((" For", 0.75, 0.95), (" example", 0.95, 1.4), (" four.", 1.4, 1.9))
    finals = c.update(again, 2.0).finals + c.update(again, 2.5).finals + c.flush()
    assert [s.text for s in finals] == ["quantum, for example four."]


def test_genuine_repeat_far_from_the_cut_is_kept() -> None:
    c = LiveCommitter(NS)
    first = words((" very", 0.0, 0.4))
    c.update(first, 0.5)
    c.update(first, 1.0)
    later = words((" very", 2.5, 2.9), (" slow.", 2.9, 3.3))
    finals = c.update(later, 3.5).finals + c.update(later, 4.0).finals + c.flush()
    assert [s.text for s in finals] == ["very", "very slow."]  # the pause splits them; both kept


def test_hallucinated_segments_are_recognised() -> None:
    # Measured on near-silent fragments (base model): "You" and "I'll see you in the next video".
    assert is_hallucination(-1.13, 0.49, [0.02])
    assert is_hallucination(-1.27, 0.52, [0.04, 0.14, 0.83, 0.13, 0.56, 0.95, 0.65])
    # Real speech from the same recording, and unsure but real speech, are kept.
    assert not is_hallucination(-0.29, 0.06, [0.36, 0.93, 0.71, 1.0, 1.0])
    assert not is_hallucination(-1.2, 0.05, [0.4, 0.45])  # low confidence, but clearly speech
    assert not is_hallucination(-1.2, 0.5, [0.9, 0.8])  # words heard confidently
    assert not is_hallucination(-2.0, 0.9, [])


def test_tail_of_the_last_word_heard_as_a_short_unsure_word_is_dropped() -> None:
    # Real faster-whisper output: "too many" then the next window opened with "Any" (0.08 s, p=0.24).
    c = LiveCommitter(NS)
    hyp = words((" too", 0.0, 0.3), (" many", 0.3, 0.6))
    c.update(hyp, 1.0)
    c.update(hyp, 1.5)
    assert c.committed_until == 0.6
    again = [Word(" Any", 0.6, 0.68, 0.24), *words((" context", 0.68, 1.1), (" switches.", 1.1, 1.7))]
    finals = c.update(again, 2.0).finals + c.update(again, 2.5).finals + c.flush()
    assert [s.text for s in finals] == ["too many context switches."]


def test_confident_short_word_at_the_cut_is_kept() -> None:
    c = LiveCommitter(NS)
    hyp = words((" Stop.", 0.0, 0.4))
    finals = c.update(hyp, 1.0).finals + c.update(hyp, 1.5).finals
    nxt = [Word(" A", 0.4, 0.5, 0.95), *words((" pause.", 0.5, 0.9))]
    finals += c.update(nxt, 2.0).finals + c.update(nxt, 2.5).finals + c.flush()
    assert [s.text for s in finals] == ["Stop.", "A pause."]


def test_jumping_over_a_gap_forgets_the_seam() -> None:
    # After a gap the next words are new speech, never a repeat of the words before the gap.
    c = LiveCommitter(NS)
    hyp = words((" we", 0.0, 0.3))
    c.update(hyp, 0.5)
    c.update(hyp, 1.0)
    c.flush(until=10.0)
    assert c.tail == [] and c.committed_until == 10.0
    after = words((" we", 10.0, 10.3), (" will.", 10.3, 10.7))
    finals = c.update(after, 11.0).finals + c.update(after, 11.5).finals + c.flush()
    assert [s.text for s in finals] == ["we will."]


def test_word_at_the_cut_reheard_with_context_audio_is_not_taken_twice() -> None:
    # With 1 s of context the next decode hears "throughput" whole (and sometimes splits it).
    c = LiveCommitter(NS)
    hyp = words((" CPU", 0.0, 0.4), (" throughput,", 0.4, 1.0))
    c.update(hyp, 1.2)
    c.update(hyp, 1.5)
    assert c.committed_until == 1.0
    again = words((" throughput,", 0.45, 1.1), (" put", 0.8, 1.05), (" waiting.", 1.3, 1.8))
    finals = c.update(again, 2.0).finals + c.update(again, 2.5).finals + c.flush()
    assert [s.text for s in finals] == ["CPU throughput, waiting."]


def test_first_word_after_silence_is_never_taken_for_a_seam_fragment() -> None:
    # Seen live: "... gives it the CPU." then silence, then "We measure" lost its short, unsure "We".
    c = LiveCommitter(NS)
    hyp = words((" the", 0.0, 0.3), (" CPU.", 0.3, 0.8))
    finals = c.update(hyp, 1.0).finals + c.update(hyp, 1.5).finals
    c.update([], 3.0)  # silence: the cut moves to 2.0, away from the end of "CPU."
    assert c.committed_until == 2.0
    nxt = [Word(" We", 2.02, 2.18, 0.3), *words((" measure", 2.18, 2.6))]
    finals += c.update(nxt, 3.5).finals + c.update(nxt, 4.0).finals + c.flush()
    assert [s.text for s in finals] == ["the CPU.", "We measure"]


def test_words_in_scripts_a_lecture_cannot_contain_are_rejected() -> None:
    # Seen in a live decode that fell back to sampling: "throughput, 邸rup 邶After 邸rup ...".
    for word in (" throughput,", " Café", " షెడ్యూలింగ్", " शेड्यूलिंग", " 4ms", " ____", " Q."):
        assert in_supported_script(word), word
    for word in (" 邸rup", " 邶After", " 失明obra", " ______且", " привет"):
        assert not in_supported_script(word), word


def test_decoding_loops_are_collapsed_to_one_copy() -> None:
    # Seen live: "The simplest algorithm is the simplest algorithm is ..." about 40 times.
    loop = [" The", " simplest", " algorithm", " is"] + [" the", " simplest", " algorithm", " is"] * 40
    looped = [Word(t, i * 0.1, i * 0.1 + 0.1, 0.9) for i, t in enumerate([*loop, " first."])]
    assert "".join(w.text for w in collapse_loops(looped)) == " The simplest algorithm is first."
    # Ordinary speech, including a word said twice, is untouched.
    speech = [Word(t, i, i + 0.5, 0.9) for i, t in enumerate([" very", " very", " slow", " very", " slow"])]
    assert collapse_loops(speech) == speech


def test_failed_fallback_samples_are_recognised() -> None:
    # Measured: sampled at T=1.0 with avg_logprob -1.11 ("wait for the next one"), -1.23 ("Henry").
    assert is_failed_fallback(1.0, -1.11) and is_failed_fallback(1.0, -1.23)
    assert not is_failed_fallback(0.8, -0.72)  # a fallback that succeeded
    assert not is_failed_fallback(0.0, -1.2)  # greedy result (left to the hallucination check)


def test_word_that_began_before_a_silence_cut_is_kept() -> None:
    # Seen live: silence moved the cut to 2.0 while "We" (1.85-2.1) was starting; it was lost.
    c = LiveCommitter(NS)
    hyp = words((" the", 0.0, 0.3), (" CPU.", 0.3, 0.8))
    finals = c.update(hyp, 1.0).finals + c.update(hyp, 1.5).finals
    c.update([], 3.0)
    assert c.committed_until == 2.0
    nxt = [Word(" We", 1.85, 2.1, 0.6), *words((" measure", 2.1, 2.6))]
    finals += c.update(nxt, 3.5).finals + c.update(nxt, 4.0).finals + c.flush()
    assert [s.text for s in finals] == ["the CPU.", "We measure"]


def test_new_word_smeared_back_across_the_cut_is_kept() -> None:
    # Seen live: after "... gives it the CPU." Whisper timed "We" 18.2-18.9 (p=0.00) across the cut
    # at 18.59. By time it looks like "CPU." heard again; by text it is new speech.
    c = LiveCommitter(NS)
    hyp = words((" the", 0.0, 0.25), (" CPU.", 0.25, 0.6))
    finals = c.update(hyp, 1.0).finals + c.update(hyp, 1.5).finals
    assert c.committed_until == 0.6
    nxt = [Word(" We", 0.2, 0.9, 0.0), *words((" measure", 0.9, 1.4))]
    finals += c.update(nxt, 2.0).finals + c.update(nxt, 2.5).finals + c.flush()
    assert [s.text for s in finals] == ["the CPU.", "We measure"]


def test_misspelled_rehearing_of_the_last_word_is_dropped() -> None:
    # Seen in a replay: "... every short process wait." then "weight." timed 38.78-39.18 (cut 39.14).
    c = LiveCommitter(NS)
    hyp = words((" process", 0.0, 0.4), (" wait.", 0.4, 0.8))
    finals = c.update(hyp, 1.0).finals + c.update(hyp, 1.5).finals
    nxt = words((" weight.", 0.45, 0.85), (" This", 1.2, 1.4), (" is.", 1.4, 1.7))
    finals += c.update(nxt, 2.0).finals + c.update(nxt, 2.5).finals + c.flush()
    assert [s.text for s in finals] == ["process wait.", "This is."]
