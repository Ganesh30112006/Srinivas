"""Encodes a 16 kHz mono 16-bit WAV into back-to-back 500 ms ADPCM chunks (4004 bytes each),
with the backend's codec, which is bit-identical to the firmware's (contracts/PROTOCOL.md 1.2).

    python make_test_clip.py in.wav out.adpcm
"""

import sys
import wave
from array import array
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "backend"))
from app.audio.adpcm import encode  # noqa: E402

CHUNK = 8000  # samples per 500 ms chunk

with wave.open(sys.argv[1], "rb") as w:
    if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
        sys.exit("need a 16 kHz mono 16-bit WAV")
    pcm = array("h", w.readframes(w.getnframes()))
pcm.extend([0] * (CHUNK - len(pcm) % CHUNK + CHUNK))  # pad to whole chunks plus 0.5-1 s of silence
out = b"".join(encode(pcm[i : i + CHUNK]) for i in range(0, len(pcm), CHUNK))
Path(sys.argv[2]).write_bytes(out)
print(f"{sys.argv[2]}: {len(pcm) // CHUNK} chunks = {len(pcm) / 16000:.1f} s, {len(out)} bytes")
