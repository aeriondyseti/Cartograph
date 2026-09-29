param(
    [ValidateSet('Render', 'Play')][string]$Action = 'Render',
    [ValidateSet('Test', 'Start', 'Finish', 'Manual', 'Abort', 'ManualBuild', 'ManualMap', 'ManualRemove', 'ManualDone', 'ManualFoundries', 'ManualAssemblers', 'ManualDismantle', 'ManualHold', 'FoundationBuild', 'FoundationMap', 'FoundationRemove', 'FoundationVerify', 'ManualReport')][string]$Cue = 'Test',
    [ValidateRange(1, 100)][int]$Volume = 40
)
$ErrorActionPreference = 'Stop'
$audioDir = Join-Path $PSScriptRoot 'AudioCues'
$manifestPath = Join-Path $audioDir 'manifest.json'
$phrases = [ordered]@{
    Test = 'Cartograph audio check. If you can hear this, please tell us whether the volume is comfortable.'
    Start = 'Cartograph test starting. Please keep the game focused until the finish cue, or our agreed finish time.'
    Finish = 'Cartograph test finished. You can switch back now.'
    Manual = 'Please prepare for the manual building test. Begin at our agreed start time.'
    Abort = 'Cartograph test stopped. You can switch back now.'
    ManualBuild = 'Manual test, part one. Place two foundries, then two assemblers. Then dismantle those same four, and stand still. Begin now.'
    ManualMap = 'Part one is recorded. Stay in the game. Part two has no timing. Place a few foundations, several at once if you like. Then open the map, check that they are there, zoom in and out, and close the map.'
    ManualRemove = 'Now dismantle those foundations. Then open the map again, check that they are gone, and close it.'
    ManualDone = 'Manual check finished. You can switch back now, and tell us what you saw.'
    ManualFoundries = 'Step one. Using your build gun, place five foundries on the clear platform. You have thirty seconds after this instruction. Keep the game focused. If you need more time or help, stop and return to the room.'
    ManualAssemblers = 'Step two. Place five assemblers on the clear platform. You have thirty seconds after this instruction.'
    ManualDismantle = 'Step three. Dismantle only the five foundries and five assemblers you just placed. You have thirty seconds. Then stand still and wait.'
    ManualHold = 'The machine building steps are finished. Stop building and stay in the game while we finish recording. You will hear when it is safe to switch back.'
    FoundationBuild = 'The timed machine test has ended. Begin the untimed map check. Extend the platform outward with four new foundations into open air. Do not stack them over existing foundations, so their outlines can be seen on the map. You have thirty seconds.'
    FoundationMap = 'Open the map with your usual key. Check that the four new foundations appear in the right place. Zoom in and out, then close the map. You have thirty seconds.'
    FoundationRemove = 'Dismantle only those four new foundations. Use mass dismantle if convenient. Leave the existing platform intact. You have thirty seconds.'
    FoundationVerify = 'Open the map again. Check that the four test foundations are gone. Zoom in and out, then close the map. You have thirty seconds.'
    ManualReport = 'The manual checks are complete. You can switch back to the room now. Tell us about any hitching, missing map updates, or test buildings you did not remove.'
}

if ($Action -eq 'Render') {
    New-Item -ItemType Directory -Path $audioDir -Force | Out-Null
    Add-Type -AssemblyName System.Speech
    $speaker = New-Object System.Speech.Synthesis.SpeechSynthesizer
    try {
        # Route to a file before Speak: preparing cues never uses the audio device.
        $speaker.SetOutputToNull()
        $speaker.Volume = $Volume
        $speaker.Rate = 0
        $items = @()
        foreach ($name in $phrases.Keys) {
            $filename = ('cue-{0}.wav' -f $name.ToLowerInvariant())
            $path = Join-Path $audioDir $filename
            try {
                $speaker.SetOutputToWaveFile($path)
                $speaker.Speak($phrases[$name])
            } finally {
                $speaker.SetOutputToNull()
            }
            $bytes = [IO.File]::ReadAllBytes($path)
            if ($bytes.Length -le 44 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'RIFF' -or
                [Text.Encoding]::ASCII.GetString($bytes, 8, 4) -ne 'WAVE') {
                throw "Invalid generated WAV: $filename"
            }
            $items += [pscustomobject]@{cue=$name; file=$filename; text=$phrases[$name]; bytes=$bytes.Length}
        }
        [ordered]@{
            generated_utc=[DateTime]::UtcNow.ToString('o')
            voice=$speaker.Voice.Name
            synthesis_volume=$Volume
            playback_verified=$false
            cues=$items
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    } finally {
        $speaker.Dispose()
    }
    exit 0
}

if (!(Test-Path -LiteralPath $manifestPath)) { throw 'Render the cue files before playback.' }
$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
$entry = @($manifest.cues | Where-Object cue -eq $Cue)
if ($entry.Count -ne 1) { throw "Cue missing from manifest: $Cue" }
# Use the fixed filename rather than interpreting a path from the manifest.
$wavePath = Join-Path $audioDir ('cue-{0}.wav' -f $Cue.ToLowerInvariant())
$player = New-Object System.Media.SoundPlayer
try {
    $player.SoundLocation = $wavePath
    $player.Load()
    $player.PlaySync()
} finally {
    $player.Dispose()
}
