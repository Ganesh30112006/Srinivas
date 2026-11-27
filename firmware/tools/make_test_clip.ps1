# Makes firmware/recorder/main/test_clip.adpcm, the speech the recorder plays when it is built with
# "Audio source = Built-in test clip" (no microphone needed). Windows 10/11 only:
#
#   powershell -ExecutionPolicy Bypass -File firmware/tools/make_test_clip.ps1
#
# It speaks test_clip.txt with a built-in voice into a 16 kHz mono 16-bit WAV, then
# make_test_clip.py encodes it with the backend's ADPCM codec into whole 500 ms chunks.
param([string]$Voice = "Microsoft David Desktop", [string]$Python = "backend\.venv\Scripts\python.exe")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
Add-Type -AssemblyName System.Speech
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SelectVoice($Voice)
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$wav = Join-Path $env:TEMP "lc_test_clip.wav"
$synth.SetOutputToWaveFile($wav, $fmt)
$synth.Speak((Get-Content "$PSScriptRoot\test_clip.txt" -Raw))
$synth.Dispose()
& (Join-Path $root $Python) -I "$PSScriptRoot\make_test_clip.py" $wav (Join-Path $root "firmware\recorder\main\test_clip.adpcm")
Remove-Item $wav
