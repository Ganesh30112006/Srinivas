# Synthetic test lectures for the evaluation scripts (Windows 10/11 only).
#
# Speaks every synthetic/*.txt with four built-in Windows voices (US English: Zira, David;
# Indian English: Heera, Ravi), converts each to 16 kHz mono 16-bit WAV (the device's format),
# and writes eval/data/manifest.jsonl for asr_benchmark.py. It also writes three concatenated
# lectures (about 3, 5 and 14 minutes) for the device simulator's outage tests.
#
#   powershell -ExecutionPolicy Bypass -File eval/synthetic/make_lectures.ps1
#
# Synthetic speech is clean and evenly paced: results on it are a lower bound for errors, not a
# substitute for real classroom recordings (see eval/README.md).
param([string]$Python = "backend\.venv\Scripts\python.exe")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
$out = Join-Path $root "eval\data\synthetic"
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Media.SpeechSynthesis.SpeechSynthesizer, Windows.Media.SpeechSynthesis, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.DataReader, Windows.Storage.Streams, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Wait-Async($op, [Type]$type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($op)); $task.Wait() | Out-Null; $task.Result
}
$synth = New-Object Windows.Media.SpeechSynthesis.SpeechSynthesizer
$voices = [ordered]@{ zira = 'Microsoft Zira'; david = 'Microsoft David'; heera = 'Microsoft Heera'; ravi = 'Microsoft Ravi' }
foreach ($txt in Get-ChildItem "$PSScriptRoot\*.txt") {
    Copy-Item $txt.FullName $out
    $text = Get-Content $txt.FullName -Raw
    foreach ($key in $voices.Keys) {
        $voice = [Windows.Media.SpeechSynthesis.SpeechSynthesizer]::AllVoices | Where-Object { $_.DisplayName -eq $voices[$key] }
        if ($null -eq $voice) { Write-Warning "$($voices[$key]) is not installed; skipped"; continue }
        $synth.Voice = $voice
        $stream = Wait-Async ($synth.SynthesizeTextToStreamAsync($text)) ([Windows.Media.SpeechSynthesis.SpeechSynthesisStream])
        $reader = New-Object Windows.Storage.Streams.DataReader($stream.GetInputStreamAt(0))
        $size = [uint32]$stream.Size
        $null = Wait-Async ($reader.LoadAsync($size)) ([uint32])
        $bytes = New-Object byte[] $size
        $reader.ReadBytes($bytes)
        [IO.File]::WriteAllBytes((Join-Path $out "$($txt.BaseName)_$key.raw.wav"), $bytes)
        "spoke $($txt.BaseName) ($key)"
    }
}
& (Join-Path $root $Python) -I (Join-Path $PSScriptRoot "finish_lectures.py") $out
