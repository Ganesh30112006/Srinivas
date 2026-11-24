"""Second half of make_lectures.ps1: resample to 16 kHz mono 16-bit, write the manifest and the
concatenated outage-test lectures. Usage: python finish_lectures.py <eval/data/synthetic>."""

from __future__ import annotations

import json
import sys
import wave
from pathlib import Path

import numpy as np
from faster_whisper.audio import decode_audio

RATE = 16_000


def main() -> None:
    out = Path(sys.argv[1])
    for raw in sorted(out.glob("*.raw.wav")):
        pcm = (np.clip(decode_audio(str(raw), sampling_rate=RATE), -1, 1) * 32767).astype("<i2")
        with wave.open(str(raw.with_name(raw.name.replace(".raw.wav", ".wav"))), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(pcm.tobytes())
        raw.unlink()
    recordings = sorted(p for p in out.glob("*_*.wav"))
    with (out.parent / "manifest.jsonl").open("w", encoding="utf-8") as manifest:
        for wav in recordings:
            topic, voice = wav.stem.split("_")
            item = {
                "audio": f"synthetic/{wav.name}",
                "reference_file": f"synthetic/{topic}.txt",
                "language": "en",
                "voice": voice,
            }
            manifest.write(json.dumps(item) + "\n")
    for name, minutes in (("rec_short", 2.5), ("rec_medium", 4.5), ("rec_long", 13)):
        frames: list[bytes] = []
        total = 0.0
        while total < minutes * 60:
            with wave.open(str(recordings[len(frames) % len(recordings)])) as w:
                frames.append(w.readframes(w.getnframes()))
                total += w.getnframes() / RATE
        with wave.open(str(out.parent / f"{name}.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(b"".join(frames))
        print(f"{name}.wav: {total / 60:.1f} min")
    print(f"{len(recordings)} recordings; manifest: {out.parent / 'manifest.jsonl'}")


if __name__ == "__main__":
    main()
