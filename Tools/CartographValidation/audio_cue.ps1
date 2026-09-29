param(
    [ValidateSet('Render', 'Play')][string]$Action = 'Render',
    [ValidateSet('Test', 'Start', 'Finish', 'Manual', 'Abort', 'ManualBuild', 'ManualMap', 'ManualRemove', 'ManualDone', 'ManualFoundries', 'ManualAssemblers', 'ManualDismantle', 'ManualHold', 'FoundationBuild', 'FoundationMap', 'FoundationRemove', 'FoundationVerify', 'ManualReport')][string]$Cue = 'Test',
    [ValidateRange(1, 100)][int]$Volume = 40
)
$ErrorActionPreference = 'Stop'
$audioDir = Join-Path $PSScriptRoot 'AudioCues'
New-Item -ItemType Directory -Path $audioDir -Force | Out-Null
$worker = Join-Path $PSScriptRoot 'audio_cue_worker.ps1'
$windowsPowerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
if (!(Test-Path -LiteralPath $windowsPowerShell)) { throw 'Windows PowerShell is required for System.Speech.' }
$operation = '{0}-{1}' -f $Action.ToLowerInvariant(), [Guid]::NewGuid().ToString('N')
$stdoutPath = Join-Path $audioDir ($operation + '.out.log')
$stderrPath = Join-Path $audioDir ($operation + '.err.log')
$arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
               '-File', ('"{0}"' -f $worker), '-Action', $Action, '-Cue', $Cue, '-Volume', $Volume)
$startedUtc = [DateTime]::UtcNow.ToString('o')
$process = Start-Process -FilePath $windowsPowerShell -ArgumentList $arguments -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -Wait -PassThru
[ordered]@{started_utc=$startedUtc; finished_utc=[DateTime]::UtcNow.ToString('o');
    action=$Action; cue=$Cue; exit_code=$process.ExitCode; operation=$operation;
    playback_requested=($Action -eq 'Play'); audibility_confirmed=$false} |
    ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $audioDir 'operations.jsonl') -Encoding UTF8
if ($process.ExitCode -ne 0) {
    $detail = Get-Content -Raw -LiteralPath $stderrPath
    throw "Audio cue $Action failed (exit $($process.ExitCode)): $detail"
}
[pscustomobject]@{action=$Action; cue=if ($Action -eq 'Play') {$Cue} else {$null};
                  completed=$true; played_audio=($Action -eq 'Play'); logs=$operation}
