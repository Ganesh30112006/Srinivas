# Makes the full-stack test recording (Windows 10/11): speaks e2e-stack/lecture.txt with a built-in
# voice, with a 1.5 s silence where the text says [pause] (it checks that a sentence starting right
# after a pause is kept), and writes a 16 kHz mono 16-bit WAV, the device's format.
#
#   powershell -ExecutionPolicy Bypass -File frontend/e2e-stack/make_recording.ps1 -Out lecture.wav
#
# -Voice: "Microsoft David" (default), "Microsoft Zira", "Microsoft Heera", "Microsoft Ravi" (Indian English).
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Voice = "Microsoft David",
    [string]$Python = "backend\.venv\Scripts\python.exe"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.SpeechSynthesis.SpeechSynthesizer, Windows.Media.SpeechSynthesis, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Wait-Async($op, [Type]$type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $task.Wait() | Out-Null; $task.Result
}
$synth = New-Object Windows.Media.SpeechSynthesis.SpeechSynthesizer
$synth.Voice = [Windows.Media.SpeechSynthesis.SpeechSynthesizer]::AllVoices | Where-Object { $_.DisplayName -eq $Voice }
if ($null -eq $synth.Voice) { throw "Voice '$Voice' is not installed" }
$text = [Security.SecurityElement]::Escape((Get-Content "$PSScriptRoot\lecture.txt" -Raw))
$text = $text -replace '\[pause\]', '<break time="1500ms"/>'
$ssml = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='$($synth.Voice.Language)'>$text</speak>"
$stream = Wait-Async ($synth.SynthesizeSsmlToStreamAsync($ssml)) ([Windows.Media.SpeechSynthesis.SpeechSynthesisStream])
$reader = New-Object Windows.Storage.Streams.DataReader($stream.GetInputStreamAt(0))
$size = [uint32]$stream.Size
$null = Wait-Async ($reader.LoadAsync($size)) ([uint32])
$bytes = New-Object byte[] $size
$reader.ReadBytes($bytes)
$raw = "$Out.raw.wav"
[IO.File]::WriteAllBytes($raw, $bytes)
# Single quotes only: Windows PowerShell 5.1 strips double quotes from native arguments.
$convert = @'
import sys, wave
import numpy as np
from faster_whisper.audio import decode_audio
pcm = (np.clip(decode_audio(sys.argv[1], sampling_rate=16000), -1, 1) * 32767).astype('<i2')
with wave.open(sys.argv[2], 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm.tobytes())
print(f'{sys.argv[2]}: {len(pcm) / 16000:.1f} s')
'@
& (Join-Path $root $Python) -I -c $convert $raw $Out
Remove-Item $raw
