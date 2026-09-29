param(
    [Parameter(Mandatory)][ValidateSet('Machines', 'MapCheck')][string]$Phase,
    [switch]$DescribeOnly
)
$ErrorActionPreference = 'Stop'
$sequence = if ($Phase -eq 'Machines') {
    @('ManualFoundries', 'ManualAssemblers', 'ManualDismantle', 'ManualHold')
} else {
    @('FoundationBuild', 'FoundationMap', 'FoundationRemove', 'FoundationVerify', 'ManualReport')
}
$stopFile = Join-Path $PSScriptRoot 'stop-manual-audio'
$helper = Join-Path $PSScriptRoot 'audio_cue.ps1'
if ($DescribeOnly) {
    $sequence | ForEach-Object { [pscustomobject]@{cue=$_;pause_after_seconds=if ($_ -in @('ManualHold','ManualReport')) {0} else {30}} }
    exit 0
}
foreach ($cueName in $sequence) {
    if (Test-Path -LiteralPath $stopFile) { throw 'Manual audio sequence stopped by operator.' }
    & $helper -Action Play -Cue $cueName
    if ($cueName -notin @('ManualHold','ManualReport')) {
        for ($second = 0; $second -lt 30; $second++) {
            if (Test-Path -LiteralPath $stopFile) { throw 'Manual audio sequence stopped by operator.' }
            Start-Sleep -Seconds 1
        }
    }
}
