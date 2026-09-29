param(
    [int]$DurationSeconds = 7200,
    [string]$OutputName = 'memory.csv'
)
$ErrorActionPreference = 'Stop'
$outputPath = Join-Path $PSScriptRoot $OutputName
if (Test-Path -LiteralPath $outputPath) { throw "Output already exists: $outputPath" }
$phasePath = Join-Path $PSScriptRoot 'phase.txt'
$stopPath = Join-Path $PSScriptRoot 'stop-monitor'
$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
$counterPaths = @('\GPU Process Memory(*)\Dedicated Usage', '\GPU Process Memory(*)\Shared Usage', '\GPU Process Memory(*)\Total Committed')
while ([DateTime]::UtcNow -lt $deadline -and !(Test-Path -LiteralPath $stopPath)) {
    $game = Get-Process -Name 'FactoryGame*' -ErrorAction SilentlyContinue | Sort-Object WorkingSet64 -Descending | Select-Object -First 1
    if (!$game) { Start-Sleep -Seconds 1; continue }
    $gpu = $null
    $gpuError = ''
    try {
        $gpu = @((Get-Counter -Counter $counterPaths -MaxSamples 1 -ErrorAction Stop).CounterSamples | Where-Object { $_.InstanceName -like "pid_$($game.Id)_*" })
    } catch { $gpuError = $_.Exception.Message; Start-Sleep -Seconds 1 }
    try { $game.Refresh(); $privateBytes = $game.PrivateMemorySize64; $workingSet = $game.WorkingSet64 } catch { continue }
    $phase = 'unlabelled'
    try {
        if (Test-Path -LiteralPath $phasePath) {
            $phaseText = [string](Get-Content -Raw -LiteralPath $phasePath)
            if ($phaseText.Trim()) { $phase = $phaseText.Trim() }
        }
    } catch { $phase = 'phase-unavailable' }
    $row = [ordered]@{utc=[DateTime]::UtcNow.ToString('o');pid=$game.Id;phase=$phase;private_bytes=$privateBytes;working_set_bytes=$workingSet}
    foreach ($metric in @(@('gpu_dedicated_bytes','dedicated usage'),@('gpu_shared_bytes','shared usage'),@('gpu_committed_bytes','total committed'))) {
        $values = @($gpu | Where-Object { $_.Path -like "*\$($metric[1])" })
        $row[$metric[0]] = if ($values.Count) { [long](($values | Measure-Object CookedValue -Sum).Sum) } else { $null }
    }
    $row['gpu_counter_error'] = $gpuError
    [pscustomobject]$row | Export-Csv -LiteralPath $outputPath -Append -NoTypeInformation
}
