# longrun.ps1 — тривалий прогін для виявлення витоків.
#
# Master Prompt (Частина 6 §36, §37) вимагає перевірити, що споживання
# не зростає поступово. Витік пам'яті чи дескрипторів виявляється лише
# на довгому інтервалі, а не за секунди роботи.
#
# Скрипт періодично знімає показники Bridge і наприкінці порівнює початок
# із кінцем. Зростання RAM або кількості дескрипторів означає витік.

[CmdletBinding()]
param(
    [int]$Minutes = 10,
    [int]$IntervalSeconds = 15,
    [string]$RelayPort = "8795"
)

$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$bridgeExe = Join-Path $root "bridge\build\claude-monitor-bridge.exe"
$relayDir = Join-Path $root "relay"
$workDir = Join-Path $env:TEMP "cm-longrun"
$outputFile = Join-Path $root "docs\longrun-results.md"

if (-not (Test-Path $workDir)) { New-Item -ItemType Directory -Force -Path $workDir | Out-Null }

Write-Host ""
Write-Host "ТРИВАЛИЙ ПРОГІН: $Minutes хв, замір кожні $IntervalSeconds с" -ForegroundColor White
Write-Host ""

Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$configPath = Join-Path $env:USERPROFILE ".claude-monitor\bridge.json"
$savedConfig = $null
if (Test-Path $configPath) { $savedConfig = Get-Content $configPath -Raw }
& $bridgeExe --set-relay "ws://127.0.0.1:$RelayPort/ws" --allow-insecure 2>&1 | Out-Null

$relay = Start-Process node -ArgumentList "src/index.js", "--dev", "--port", $RelayPort, "--log-level", "error" `
    -WorkingDirectory $relayDir -PassThru -NoNewWindow `
    -RedirectStandardOutput "$workDir\relay.log" -RedirectStandardError "$workDir\relay-err.log"
Start-Sleep -Seconds 2

$bridge = Start-Process $bridgeExe -ArgumentList "--log-level", "warn" -PassThru -NoNewWindow `
    -RedirectStandardOutput "$workDir\bridge.log" -RedirectStandardError "$workDir\bridge-err.log"
Start-Sleep -Seconds 4

if ($bridge.HasExited) {
    Write-Host "Bridge не запустився" -ForegroundColor Red
    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    exit 2
}

$samples = New-Object System.Collections.Generic.List[object]
$deadline = (Get-Date).AddMinutes($Minutes)
$restarts = 0

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $IntervalSeconds

    if ($bridge.HasExited) {
        # Аварійне завершення посеред прогону — це вже провал.
        Write-Host "Bridge завершився передчасно!" -ForegroundColor Red
        $restarts++
        break
    }

    $bridge.Refresh()
    $sample = [PSCustomObject]@{
        Time     = Get-Date
        RssMb    = [Math]::Round($bridge.WorkingSet64 / 1MB, 2)
        Handles  = $bridge.HandleCount
        Threads  = $bridge.Threads.Count
        CpuSec   = [Math]::Round($bridge.TotalProcessorTime.TotalSeconds, 2)
    }
    $samples.Add($sample)

    $elapsed = [Math]::Round(((Get-Date) - $samples[0].Time).TotalMinutes, 1)
    Write-Host ("  {0,5:N1} хв   RAM {1,6:N2} МБ   дескрипторів {2,4}   потоків {3,2}   CPU {4,6:N2} с" -f `
        $elapsed, $sample.RssMb, $sample.Handles, $sample.Threads, $sample.CpuSec)
}

Stop-Process -Id $bridge.Id -Force -ErrorAction SilentlyContinue
Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
if ($savedConfig) { Set-Content -Path $configPath -Value $savedConfig -Encoding UTF8 -NoNewline }

# ── Аналіз ───────────────────────────────────────────────────────────────────

if ($samples.Count -lt 3) {
    Write-Host "Замало замірів для висновку" -ForegroundColor Yellow
    exit 1
}

$first = $samples[0]
$last = $samples[$samples.Count - 1]
$durationMin = [Math]::Round(($last.Time - $first.Time).TotalMinutes, 1)

$rssGrowth = [Math]::Round($last.RssMb - $first.RssMb, 2)
$handleGrowth = $last.Handles - $first.Handles
$threadGrowth = $last.Threads - $first.Threads
$rssMax = ($samples | Measure-Object -Property RssMb -Maximum).Maximum
$cpuTotal = $last.CpuSec - $first.CpuSec
$cpuPercent = if ($durationMin -gt 0) {
    [Math]::Round($cpuTotal / ($durationMin * 60) * 100 / [Environment]::ProcessorCount, 4)
} else { 0 }

Write-Host ""
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor DarkGray
Write-Host "ПІДСУМОК ЗА $durationMin ХВ" -ForegroundColor White
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor DarkGray
Write-Host ("  RAM:           {0,6:N2} МБ -> {1,6:N2} МБ   (зміна {2,+6:N2} МБ, пік {3:N2})" -f `
    $first.RssMb, $last.RssMb, $rssGrowth, $rssMax)
Write-Host ("  Дескриптори:   {0,6} -> {1,6}   (зміна {2,+6})" -f `
    $first.Handles, $last.Handles, $handleGrowth)
Write-Host ("  Потоки:        {0,6} -> {1,6}   (зміна {2,+6})" -f `
    $first.Threads, $last.Threads, $threadGrowth)
Write-Host ("  CPU за період: {0,6:N2} с   ({1:N4}%)" -f $cpuTotal, $cpuPercent)
Write-Host ""

# Пороги підібрані так, щоб не реагувати на звичайні коливання роботи
# розподільника пам'яті, але помітити систематичне зростання.
$problems = @()
if ($rssGrowth -gt 5) { $problems += "RAM зросла на $rssGrowth МБ — ознака витоку" }
if ($handleGrowth -gt 50) { $problems += "дескрипторів побільшало на $handleGrowth — ознака витоку" }
if ($threadGrowth -gt 2) { $problems += "потоків побільшало на $threadGrowth" }
if ($restarts -gt 0) { $problems += "Bridge завершився передчасно" }

$verdict = if ($problems.Count -eq 0) { "ПРОЙДЕНО" } else { "ПРОВАЛЕНО" }
$color = if ($problems.Count -eq 0) { "Green" } else { "Red" }
Write-Host "  ВЕРДИКТ: $verdict" -ForegroundColor $color
foreach ($p in $problems) { Write-Host "    - $p" -ForegroundColor Red }
Write-Host ""

# ── Звіт ─────────────────────────────────────────────────────────────────────

$report = @()
$report += "# Тривалий прогін"
$report += ""
$report += "> Згенеровано скриптом ``scripts/longrun.ps1``."
$report += "> Перевіряє вимогу Частини 6 §36–37 Master Prompt: споживання не має"
$report += "> зростати з часом."
$report += ""
$report += "Дата: $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
$report += "Тривалість: $durationMin хв, замірів: $($samples.Count)"
$report += ""
$report += "## Підсумок"
$report += ""
$report += "| Показник | Початок | Кінець | Зміна |"
$report += "|---|---|---|---|"
$report += "| RAM | $($first.RssMb) МБ | $($last.RssMb) МБ | $rssGrowth МБ |"
$report += "| Дескриптори | $($first.Handles) | $($last.Handles) | $handleGrowth |"
$report += "| Потоки | $($first.Threads) | $($last.Threads) | $threadGrowth |"
$report += ""
$report += "Пікова RAM: $rssMax МБ. CPU за період: $cpuTotal с ($cpuPercent%)."
$report += ""
$report += "**Вердикт: $verdict**"
if ($problems.Count -gt 0) {
    $report += ""
    foreach ($p in $problems) { $report += "- $p" }
}
$report += ""
$report += "## Заміри"
$report += ""
$report += "| Час | RAM, МБ | Дескриптори | Потоки | CPU, с |"
$report += "|---|---|---|---|---|"
foreach ($s in $samples) {
    $report += "| $($s.Time.ToString('HH:mm:ss')) | $($s.RssMb) | $($s.Handles) | $($s.Threads) | $($s.CpuSec) |"
}

[System.IO.File]::WriteAllText($outputFile, ($report -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Звіт збережено: $outputFile" -ForegroundColor Green

if ($problems.Count -gt 0) { exit 1 }
exit 0
