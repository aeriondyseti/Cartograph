param([int]$DurationSeconds = 7200, [string]$OutputName = 'gpu-utilization.csv')
$ErrorActionPreference = 'Stop'
$outputPath = Join-Path $PSScriptRoot $OutputName
if (Test-Path -LiteralPath $outputPath) { throw "Output already exists: $outputPath" }
$gpuTool = (Get-Command nvidia-smi -ErrorAction Stop).Source
$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
$phasePath = Join-Path $PSScriptRoot 'phase.txt'
while ([DateTime]::UtcNow -lt $deadline -and !(Test-Path (Join-Path $PSScriptRoot 'stop-gpu-monitor'))) {
    $phase = 'unlabelled'
    try {
        $phaseText = [string](Get-Content -Raw -LiteralPath $phasePath -ErrorAction Stop)
        if ($phaseText.Trim()) { $phase = $phaseText.Trim() }
    } catch {}
    $lines = & $gpuTool --query-gpu=timestamp,index,name,utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits
    if ($LASTEXITCODE -eq 0) {
        foreach ($sample in ($lines | ConvertFrom-Csv -Header timestamp,index,name,utilization,memory_used,memory_total)) {
            $row = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); phase = $phase; gpu_index = $sample.index.Trim(); gpu_name = $sample.name.Trim() }
            foreach ($metric in @(@('gpu_utilization_percent','utilization'),@('memory_used_mib','memory_used'),@('memory_total_mib','memory_total'))) {
                $number = 0.0
                $row[$metric[0]] = if ([double]::TryParse($sample.($metric[1]).Trim(), [ref]$number)) { $number } else { $null }
            }
            [pscustomobject]$row | Export-Csv -LiteralPath $outputPath -Append -NoTypeInformation
        }
    }
    Start-Sleep -Seconds 1
}
