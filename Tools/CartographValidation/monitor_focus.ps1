param([int]$DurationSeconds = 3600, [string]$OutputName = 'focus.csv')
$ErrorActionPreference = 'Stop'
$outputPath = Join-Path $PSScriptRoot $OutputName
if (Test-Path -LiteralPath $outputPath) { throw "Output already exists: $outputPath" }
# Read-only window identity; no activation, input, titles, or screenshots.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CartographFocusProbe {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
}
'@
$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
while ([DateTime]::UtcNow -lt $deadline -and !(Test-Path (Join-Path $PSScriptRoot 'stop-focus-monitor'))) {
    $game = Get-Process -Name 'FactoryGame*' -ErrorAction SilentlyContinue |
        Sort-Object WorkingSet64 -Descending | Select-Object -First 1
    $foregroundProcessId = [uint32]0
    $window = [CartographFocusProbe]::GetForegroundWindow()
    if ($window -ne [IntPtr]::Zero) {
        [void][CartographFocusProbe]::GetWindowThreadProcessId($window, [ref]$foregroundProcessId)
    }
    $phase = 'unlabelled'
    try { $phase = [string](Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'phase.txt')) } catch {}
    [pscustomobject][ordered]@{
        utc=[DateTime]::UtcNow.ToString('o')
        phase=$phase.Trim()
        game_pid=if ($game) {$game.Id} else {$null}
        foreground_pid=if ($foregroundProcessId) {$foregroundProcessId} else {$null}
        game_is_foreground=if ($game -and $foregroundProcessId) {$game.Id -eq $foregroundProcessId} else {$null}
    } | Export-Csv -LiteralPath $outputPath -Append -NoTypeInformation
    Start-Sleep -Seconds 1
}
