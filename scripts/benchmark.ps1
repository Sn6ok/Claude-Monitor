# benchmark.ps1 — вимірювання споживання ресурсів Claude Monitor.
#
# Master Prompt (Частина 5 §28–30, Частина 6 §34) вимагає НЕ робити висновок
# «легкий» без вимірювань. Цей скрипт міряє фактичні цифри в кількох режимах
# і зберігає звіт.
#
# Вимірюється саме приріст: скільки монітор додає до системи, а не абсолютні
# значення, які залежать від усього іншого на комп'ютері.

[CmdletBinding()]
param(
    [int]$SampleSeconds = 60,
    [string]$RelayPort = "8796",
    [string]$OutputFile
)

$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$bridgeExe = Join-Path $root "bridge\build\claude-monitor-bridge.exe"
$relayDir = Join-Path $root "relay"
$workDir = Join-Path $env:TEMP "cm-benchmark"

if (-not (Test-Path $workDir)) { New-Item -ItemType Directory -Force -Path $workDir | Out-Null }
if (-not $OutputFile) { $OutputFile = Join-Path $root "docs\benchmark-results.md" }

# ── Вимірювання ──────────────────────────────────────────────────────────────

function Measure-Process {
    param([System.Diagnostics.Process]$Process, [int]$Seconds, [string]$Label)

    if (-not $Process -or $Process.HasExited) {
        return [PSCustomObject]@{ Label = $Label; Error = "процес не працює" }
    }

    $Process.Refresh()
    $cpuStart = $Process.TotalProcessorTime
    $rssStart = $Process.WorkingSet64
    $ioStart = (Get-CimInstance Win32_Process -Filter "ProcessId=$($Process.Id)" -ErrorAction SilentlyContinue)

    $rssSamples = New-Object System.Collections.Generic.List[double]
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

    while ($stopwatch.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 500
        if ($Process.HasExited) { break }
        $Process.Refresh()
        $rssSamples.Add($Process.WorkingSet64 / 1MB)
    }
    $stopwatch.Stop()

    if ($Process.HasExited) {
        return [PSCustomObject]@{ Label = $Label; Error = "процес завершився під час вимірювання" }
    }

    $Process.Refresh()
    $cpuUsed = ($Process.TotalProcessorTime - $cpuStart).TotalSeconds
    $elapsed = $stopwatch.Elapsed.TotalSeconds
    $cores = [Environment]::ProcessorCount

    $ioEnd = (Get-CimInstance Win32_Process -Filter "ProcessId=$($Process.Id)" -ErrorAction SilentlyContinue)
    $ioBytes = 0
    if ($ioStart -and $ioEnd) {
        $ioBytes = ($ioEnd.ReadTransferCount - $ioStart.ReadTransferCount) +
                   ($ioEnd.WriteTransferCount - $ioStart.WriteTransferCount)
    }

    [PSCustomObject]@{
        Label       = $Label
        RssStartMb  = [Math]::Round($rssStart / 1MB, 2)
        RssEndMb    = [Math]::Round($Process.WorkingSet64 / 1MB, 2)
        RssPeakMb   = [Math]::Round(($rssSamples | Measure-Object -Maximum).Maximum, 2)
        RssAvgMb    = [Math]::Round(($rssSamples | Measure-Object -Average).Average, 2)
        CpuSeconds  = [Math]::Round($cpuUsed, 3)
        # Відсоток від одного ядра, усереднений за час вимірювання.
        CpuPercent  = [Math]::Round(($cpuUsed / $elapsed) * 100 / $cores, 3)
        Threads     = $Process.Threads.Count
        Handles     = $Process.HandleCount
        IoKb        = [Math]::Round($ioBytes / 1KB, 1)
        Seconds     = [Math]::Round($elapsed, 1)
        Error       = $null
    }
}

function Format-Row {
    param($Measurement)
    if ($Measurement.Error) { return "  $($Measurement.Label): $($Measurement.Error)" }
    "  {0,-22} RAM {1,6:N1} МБ (пік {2,6:N1})  CPU {3,6:N3}%  потоків {4,2}  I/O {5,7:N1} КБ" -f `
        $Measurement.Label, $Measurement.RssAvgMb, $Measurement.RssPeakMb,
        $Measurement.CpuPercent, $Measurement.Threads, $Measurement.IoKb
}

# ── Підготовка ───────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "БЕНЧМАРК CLAUDE MONITOR" -ForegroundColor White
Write-Host "Кожен режим міряється $SampleSeconds с" -ForegroundColor DarkGray
Write-Host ""

if (-not (Test-Path $bridgeExe)) {
    Write-Host "Bridge не зібрано: $bridgeExe" -ForegroundColor Red
    exit 2
}

Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

# Тимчасово переспрямовуємо Bridge на власний Relay.
$configPath = Join-Path $env:USERPROFILE ".claude-monitor\bridge.json"
$savedConfig = $null
if (Test-Path $configPath) { $savedConfig = Get-Content $configPath -Raw }
& $bridgeExe --set-relay "ws://127.0.0.1:$RelayPort/ws" --allow-insecure 2>&1 | Out-Null

$sessionsCount = @(Get-ChildItem (Join-Path $env:USERPROFILE ".claude\sessions") -Filter "*.json" -ErrorAction SilentlyContinue).Count
$measurements = @()

# ── Режим 1: без мережі ──────────────────────────────────────────────────────
#
# Найважливіший сценарій: Relay недоступний. Черга подій наповнюється,
# але пам'ять не має рости необмежено (Частина 5 §16 Master Prompt).

Write-Host "Режим 1: Relay недоступний (перевірка обмеженості черги)" -ForegroundColor Cyan
$bridge = Start-Process $bridgeExe -ArgumentList "--log-level", "warn" -PassThru -NoNewWindow `
    -RedirectStandardOutput "$workDir\b1.log" -RedirectStandardError "$workDir\b1-err.log"
Start-Sleep -Seconds 3

$m = Measure-Process -Process $bridge -Seconds $SampleSeconds -Label "Bridge без мережі"
$measurements += $m
Write-Host (Format-Row $m)

Stop-Process -Id $bridge.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

# ── Режим 2: підключено до Relay ─────────────────────────────────────────────

Write-Host ""
Write-Host "Режим 2: підключено до Relay, монітор не приєднаний" -ForegroundColor Cyan
$relay = Start-Process node -ArgumentList "src/index.js", "--dev", "--port", $RelayPort, "--log-level", "error" `
    -WorkingDirectory $relayDir -PassThru -NoNewWindow `
    -RedirectStandardOutput "$workDir\relay.log" -RedirectStandardError "$workDir\relay-err.log"
Start-Sleep -Seconds 2

$bridge = Start-Process $bridgeExe -ArgumentList "--log-level", "warn" -PassThru -NoNewWindow `
    -RedirectStandardOutput "$workDir\b2.log" -RedirectStandardError "$workDir\b2-err.log"
Start-Sleep -Seconds 4

$m = Measure-Process -Process $bridge -Seconds $SampleSeconds -Label "Bridge підключений"
$measurements += $m
Write-Host (Format-Row $m)

$mRelay = Measure-Process -Process $relay -Seconds 5 -Label "Relay"
$measurements += $mRelay
Write-Host (Format-Row $mRelay)

# ── Режим 3: із приєднаним монітором ─────────────────────────────────────────

Write-Host ""
Write-Host "Режим 3: монітор приєднаний, події передаються" -ForegroundColor Cyan

$monitorScript = Join-Path $relayDir "tools\test-monitor.mjs"
$monitor = $null
if (Test-Path (Join-Path $env:USERPROFILE ".claude-monitor\test-monitor\identity.json")) {
    $monitor = Start-Process node -ArgumentList $monitorScript, "watch", "ws://127.0.0.1:$RelayPort/ws" `
        -WorkingDirectory $relayDir -PassThru -NoNewWindow `
        -RedirectStandardOutput "$workDir\monitor.log" -RedirectStandardError "$workDir\monitor-err.log"
    Start-Sleep -Seconds 4

    $m = Measure-Process -Process $bridge -Seconds $SampleSeconds -Label "Bridge + монітор"
    $measurements += $m
    Write-Host (Format-Row $m)

    # Скільки трафіку згенеровано за час вимірювання.
    $monitorSize = 0
    if (Test-Path "$workDir\monitor.log") {
        $monitorSize = (Get-Item "$workDir\monitor.log").Length
    }
    Write-Host ("  отримано моніторм: {0:N1} КБ за {1} с" -f ($monitorSize / 1KB), $SampleSeconds)

    Stop-Process -Id $monitor.Id -Force -ErrorAction SilentlyContinue
} else {
    Write-Host "  пропущено: тестовий монітор не спарений" -ForegroundColor Yellow
}

Stop-Process -Id $bridge.Id -Force -ErrorAction SilentlyContinue
Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue

# ── Розмір артефактів ────────────────────────────────────────────────────────

$bridgeSize = (Get-Item $bridgeExe).Length / 1KB
$apkPath = Join-Path $root "android\app\build\outputs\apk\release\app-release.apk"
$apkSize = if (Test-Path $apkPath) { (Get-Item $apkPath).Length / 1MB } else { 0 }

# Повертаємо конфігурацію користувача.
if ($savedConfig) { Set-Content -Path $configPath -Value $savedConfig -Encoding UTF8 -NoNewline }

# ── Звіт ─────────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor DarkGray
Write-Host "РЕЗУЛЬТАТИ" -ForegroundColor White
Write-Host "════════════════════════════════════════════════════════════" -ForegroundColor DarkGray
Write-Host ("  Розмір Bridge:  {0,8:N0} КБ" -f $bridgeSize)
Write-Host ("  Розмір APK:     {0,8:N2} МБ" -f $apkSize)
Write-Host ("  Активних сесій під час вимірювання: {0}" -f $sessionsCount)
Write-Host ""

$report = @()
$report += "# Результати вимірювання ресурсів"
$report += ""
$report += "> Згенеровано автоматично скриптом ``scripts/benchmark.ps1``."
$report += "> Це фактичні виміри, а не оцінки (Частина 5 §28, Частина 6 §34 Master Prompt)."
$report += ""
$report += "Дата: $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
$report += "Тривалість кожного режиму: $SampleSeconds с"
$report += "Активних сесій Claude Code: $sessionsCount"
$report += "Процесорних ядер: $([Environment]::ProcessorCount)"
$report += ""
$report += "## Споживання"
$report += ""
$report += "| Режим | RAM середня | RAM пік | CPU | Потоків | Дескрипторів | Диск |"
$report += "|---|---|---|---|---|---|---|"

foreach ($m in $measurements) {
    if ($m.Error) {
        $report += "| $($m.Label) | — | — | — | — | — | $($m.Error) |"
    } else {
        $report += ("| {0} | {1:N1} МБ | {2:N1} МБ | {3:N3}% | {4} | {5} | {6:N1} КБ |" -f `
            $m.Label, $m.RssAvgMb, $m.RssPeakMb, $m.CpuPercent, $m.Threads, $m.Handles, $m.IoKb)
    }
}

$report += ""
$report += "## Розмір артефактів"
$report += ""
$report += "| Артефакт | Розмір |"
$report += "|---|---|"
$report += ("| ``claude-monitor-bridge.exe`` | {0:N0} КБ |" -f $bridgeSize)
$report += ("| ``app-release.apk`` | {0:N2} МБ |" -f $apkSize)
$report += ""
$report += "## Як читати ці цифри"
$report += ""
$report += "**RAM пік** важливіший за середню: саме він показує, чи не росте"
$report += "споживання необмежено. Режим «без мережі» перевіряє найгірший випадок —"
$report += "черга подій наповнюється, а віддати їх нікуди."
$report += ""
$report += "**CPU** наведено у відсотках від усього процесора, а не від одного ядра."
$report += "Значення біля нуля означає, що процес майже весь час спить на об'єктах"
$report += "ядра, а не опитує систему в циклі."
$report += ""
$report += "**Дескриптори** й **потоків** сталі: їх зростання з часом означало б витік."

$reportText = $report -join "`r`n"
$utf8Bom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($OutputFile, $reportText, $utf8Bom)

Write-Host "Звіт збережено: $OutputFile" -ForegroundColor Green
Write-Host ""
