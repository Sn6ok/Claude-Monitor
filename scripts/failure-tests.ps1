# failure-tests.ps1 — тести відмов Claude Monitor.
#
# Перевіряють головну вимогу Master Prompt (Частина 6 §28, §29):
#
#   ЩО Б НЕ СТАЛОСЯ З МОНІТОРИНГОМ, CLAUDE CODE ПРОДОВЖУЄ ПРАЦЮВАТИ.
#
# Кожен сценарій навмисно ламає щось у системі моніторингу й перевіряє,
# що Claude Desktop і сесії Claude Code лишилися неушкодженими.

[CmdletBinding()]
param(
    [string]$RelayPort = "8797",
    [switch]$KeepRunning
)

$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$bridgeExe = Join-Path $root "bridge\build\claude-monitor-bridge.exe"
$relayDir = Join-Path $root "relay"
$workDir = Join-Path $env:TEMP "cm-failure-tests"
# Bridge пише журнал у власний каталог даних, а не в stdout.
$bridgeLog = Join-Path $env:USERPROFILE ".claude-monitor\bridge.log"

if (-not (Test-Path $workDir)) { New-Item -ItemType Directory -Force -Path $workDir | Out-Null }

$script:passed = 0
$script:failed = 0
$script:results = @()

function Test-Case {
    param([string]$Name, [scriptblock]$Body)

    Write-Host ""
    Write-Host "── $Name " -NoNewline -ForegroundColor Cyan
    Write-Host ("─" * [Math]::Max(0, 60 - $Name.Length)) -ForegroundColor DarkGray

    try {
        $result = & $Body
        if ($result -eq $true) {
            Write-Host "   ПРОЙДЕНО" -ForegroundColor Green
            $script:passed++
            $script:results += [PSCustomObject]@{ Test = $Name; Result = "ПРОЙДЕНО" }
        } else {
            Write-Host "   ПРОВАЛЕНО: $result" -ForegroundColor Red
            $script:failed++
            $script:results += [PSCustomObject]@{ Test = $Name; Result = "ПРОВАЛЕНО" }
        }
    } catch {
        Write-Host "   ПОМИЛКА: $_" -ForegroundColor Red
        $script:failed++
        $script:results += [PSCustomObject]@{ Test = $Name; Result = "ПОМИЛКА" }
    }
}

# ── Спостереження за Claude Code ─────────────────────────────────────────────
#
# Знімок стану сесій Claude Code. Саме він доводить, що моніторинг
# не зачепив нічого важливого.

function Get-ClaudeSnapshot {
    $desktop = Get-CimInstance Win32_Process -Filter "Name='claude.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -like "*WindowsApps*" }

    $sessionsDir = Join-Path $env:USERPROFILE ".claude\sessions"
    $sessions = @()
    if (Test-Path $sessionsDir) {
        $sessions = Get-ChildItem $sessionsDir -Filter "*.json" -ErrorAction SilentlyContinue |
            ForEach-Object {
                $data = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json -ErrorAction SilentlyContinue
                if ($data -and $data.pid) {
                    $alive = $null -ne (Get-Process -Id $data.pid -ErrorAction SilentlyContinue)
                    if ($alive) { $data.sessionId }
                }
            }
    }

    # Розмір транскриптів: якщо він зростає, Claude Code продовжує працювати.
    $projectsDir = Join-Path $env:USERPROFILE ".claude\projects"
    $totalBytes = 0
    if (Test-Path $projectsDir) {
        $totalBytes = (Get-ChildItem $projectsDir -Recurse -Filter "*.jsonl" -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
    }

    [PSCustomObject]@{
        DesktopCount = @($desktop).Count
        Sessions     = @($sessions)
        TotalBytes   = [long]$totalBytes
    }
}

function Assert-ClaudeUnaffected {
    param($Before, [string]$What)

    $after = Get-ClaudeSnapshot

    if ($after.DesktopCount -eq 0 -and $Before.DesktopCount -gt 0) {
        return "Claude Desktop зник після: $What"
    }

    $lost = $Before.Sessions | Where-Object { $_ -notin $after.Sessions }
    if ($lost) {
        return "зникли сесії Claude Code ($($lost -join ', ')) після: $What"
    }

    return $true
}

# ── Допоміжне ────────────────────────────────────────────────────────────────

function Start-Relay {
    $proc = Start-Process node `
        -ArgumentList "src/index.js", "--dev", "--port", $RelayPort, "--log-level", "warn" `
        -WorkingDirectory $relayDir -PassThru -NoNewWindow `
        -RedirectStandardOutput "$workDir\relay.log" -RedirectStandardError "$workDir\relay-err.log"
    Start-Sleep -Seconds 2
    return $proc
}

function Start-Bridge {
    $proc = Start-Process $bridgeExe -ArgumentList "--log-level", "info" `
        -PassThru -NoNewWindow `
        -RedirectStandardOutput "$workDir\bridge.log" -RedirectStandardError "$workDir\bridge-err.log"
    Start-Sleep -Seconds 3
    return $proc
}

function Stop-All {
    Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

function Get-BridgeProcess {
    Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue | Select-Object -First 1
}

# ── Початок ──────────────────────────────────────────────────────────────────

Write-Host ""
Write-Host "ТЕСТИ ВІДМОВ CLAUDE MONITOR" -ForegroundColor White
Write-Host "Головна вимога: Claude Code продовжує працювати за будь-яких відмов" -ForegroundColor DarkGray

if (-not (Test-Path $bridgeExe)) {
    Write-Host "Bridge не зібрано: $bridgeExe" -ForegroundColor Red
    exit 2
}

# Bridge читає адресу Relay з конфігурації. Тести піднімають власний
# Relay на іншому порту, тож конфігурацію треба тимчасово переспрямувати,
# а наприкінці — повернути як було.
$configPath = Join-Path $env:USERPROFILE ".claude-monitor\bridge.json"
$savedConfig = $null
if (Test-Path $configPath) { $savedConfig = Get-Content $configPath -Raw }

& $bridgeExe --set-relay "ws://127.0.0.1:$RelayPort/ws" --allow-insecure 2>&1 | Out-Null

$baseline = Get-ClaudeSnapshot
Write-Host ""
Write-Host "Початковий стан: Claude Desktop процесів = $($baseline.DesktopCount), активних сесій = $($baseline.Sessions.Count)"

if ($baseline.DesktopCount -eq 0) {
    Write-Host "УВАГА: Claude Desktop не запущено. Тести перевірять лише стійкість Bridge." -ForegroundColor Yellow
}

Stop-All

# ── 1. Аварійне завершення Bridge ────────────────────────────────────────────

Test-Case "Аварійне завершення Bridge не зачіпає Claude Code" {
    $relay = Start-Relay
    $bridge = Start-Bridge
    $before = Get-ClaudeSnapshot

    $proc = Get-BridgeProcess
    if (-not $proc) { return "Bridge не запустився" }

    # Жорстке вбивство без можливості коректно завершитись — найгірший випадок.
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Seconds 3

    $verdict = Assert-ClaudeUnaffected $before "аварійного завершення Bridge"
    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    return $verdict
}

# ── 2. Relay недоступний ─────────────────────────────────────────────────────

Test-Case "Недоступний Relay не зачіпає Claude Code" {
    # Bridge стартує, коли Relay взагалі немає.
    $before = Get-ClaudeSnapshot
    $bridge = Start-Bridge
    Start-Sleep -Seconds 8

    $proc = Get-BridgeProcess
    if (-not $proc) { return "Bridge завершився без Relay замість того, щоб чекати" }

    $verdict = Assert-ClaudeUnaffected $before "роботи без Relay"
    Stop-All
    return $verdict
}

# ── 3. Relay падає посеред роботи ────────────────────────────────────────────

Test-Case "Падіння Relay посеред роботи" {
    $relay = Start-Relay
    $bridge = Start-Bridge
    $before = Get-ClaudeSnapshot

    Stop-Process -Id $relay.Id -Force
    Start-Sleep -Seconds 6

    $proc = Get-BridgeProcess
    if (-not $proc) { return "Bridge завершився після падіння Relay" }

    $verdict = Assert-ClaudeUnaffected $before "падіння Relay"
    Stop-All
    return $verdict
}

# ── 4. Relay повертається ────────────────────────────────────────────────────

Test-Case "Bridge відновлює зв'язок після повернення Relay" {
    $bridge = Start-Bridge
    Start-Sleep -Seconds 5

    $relay = Start-Relay
    # Пауза з запасом: backoff міг дійти до кількох секунд.
    Start-Sleep -Seconds 20

    # Читаємо саме файл журналу Bridge, а не його stdout: у робочому
    # режимі Bridge пише журнал у власний каталог даних.
    #
    # -Encoding UTF8 обов'язковий: без нього PowerShell 5.1 тлумачить файл
    # як ANSI, кирилиця спотворюється, і пошук за шаблоном не спрацьовує.
    $tail = Get-Content $bridgeLog -Tail 40 -Encoding UTF8 -ErrorAction SilentlyContinue
    $log = ($tail -join "`n")
    $reconnected = $log -match "автентифіковано на Relay"

    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    Stop-All

    if ($reconnected) { return $true }
    return "у журналі немає ознак відновлення зв'язку"
}

# ── 5. Захист від дублікатів ─────────────────────────────────────────────────

Test-Case "Другий екземпляр Bridge не запускається" {
    $relay = Start-Relay
    $first = Start-Bridge

    $second = Start-Process $bridgeExe -ArgumentList "--log-level", "info" `
        -PassThru -NoNewWindow -RedirectStandardOutput "$workDir\bridge2.log" `
        -RedirectStandardError "$workDir\bridge2-err.log"
    Start-Sleep -Seconds 3

    $count = @(Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue).Count

    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    Stop-All

    if ($count -le 1) { return $true }
    return "працює $count екземплярів замість одного"
}

# ── 6. Сміттєві дані від Relay ───────────────────────────────────────────────

Test-Case "Сміттєві пакети не валять Bridge" {
    $relay = Start-Relay
    $bridge = Start-Bridge
    $before = Get-ClaudeSnapshot

    # Надсилаємо Relay потік некоректних даних: він має їх відхилити,
    # а Bridge — навіть не помітити.
    $garbageScript = @'
const { WebSocket } = require("ws");
const payloads = ["не json", "{", "[]", "null", '{"v":999}', "x".repeat(100000)];
let done = 0;
for (const payload of payloads) {
  const ws = new WebSocket(process.argv[2]);
  ws.on("open", () => { try { ws.send(payload); } catch {} setTimeout(() => ws.close(), 100); });
  ws.on("error", () => {});
  ws.on("close", () => { if (++done === payloads.length) process.exit(0); });
}
setTimeout(() => process.exit(0), 5000);
'@
    $garbageScript | Out-File "$workDir\garbage.js" -Encoding utf8
    & node "$workDir\garbage.js" "ws://127.0.0.1:$RelayPort/ws" 2>&1 | Out-Null
    Start-Sleep -Seconds 3

    $bridgeAlive = $null -ne (Get-BridgeProcess)
    $relayAlive = $null -ne (Get-Process -Id $relay.Id -ErrorAction SilentlyContinue)

    $verdict = Assert-ClaudeUnaffected $before "сміттєвих пакетів"

    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    Stop-All

    if ($verdict -ne $true) { return $verdict }
    if (-not $relayAlive) { return "Relay впав від сміттєвих даних" }
    if (-not $bridgeAlive) { return "Bridge впав від сміттєвих даних" }
    return $true
}

# ── 7. Хук без робочого Bridge ───────────────────────────────────────────────

Test-Case "Хук завершується миттєво, коли Bridge не працює" {
    Stop-All
    $before = Get-ClaudeSnapshot

    # Це найважливіший тест впливу на Claude Code: Claude чекає завершення
    # хука, тож той мусить повертати керування навіть без Bridge.
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $json = '{"session_id":"test","transcript_path":"","cwd":"C:\\"}'
    $json | & $bridgeExe --hook session-start 2>&1 | Out-Null
    $stopwatch.Stop()

    $elapsed = $stopwatch.ElapsedMilliseconds
    Write-Host "   час виконання хука: $elapsed мс" -ForegroundColor DarkGray

    $verdict = Assert-ClaudeUnaffected $before "виклику хука без Bridge"
    if ($verdict -ne $true) { return $verdict }

    # Поріг великий свідомо: навіть 3 секунди були б помітні, але не
    # фатальні. Реально має бути кілька мілісекунд.
    if ($elapsed -gt 3000) { return "хук виконувався $elapsed мс — це затримує Claude Code" }
    return $true
}

# ── 8. Робота під час активної роботи Claude Code ────────────────────────────

Test-Case "Транскрипти продовжують зростати під наглядом Bridge" {
    $relay = Start-Relay
    $bridge = Start-Bridge

    $before = Get-ClaudeSnapshot
    Start-Sleep -Seconds 10
    $after = Get-ClaudeSnapshot

    Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
    Stop-All

    # Якщо Claude Code працює, транскрипти дописуються. Якщо він простоює —
    # розмір не змінюється, і це теж нормально. Головне, щоб не зменшився:
    # це означало б пошкодження файлів.
    if ($after.TotalBytes -lt $before.TotalBytes) {
        return "розмір транскриптів зменшився — можливе пошкодження"
    }
    return $true
}

# ── Підсумок ─────────────────────────────────────────────────────────────────

$final = Get-ClaudeSnapshot

# Повертаємо конфігурацію користувача.
if ($savedConfig) {
    Set-Content -Path $configPath -Value $savedConfig -Encoding UTF8 -NoNewline
}

Write-Host ""
Write-Host ("═" * 64) -ForegroundColor DarkGray
Write-Host "ПІДСУМОК" -ForegroundColor White
Write-Host ("═" * 64) -ForegroundColor DarkGray
$script:results | Format-Table -AutoSize | Out-String | Write-Host

Write-Host "Пройдено: $script:passed   Провалено: $script:failed"
Write-Host ""
Write-Host "Claude Desktop до тестів:    $($baseline.DesktopCount) процесів, $($baseline.Sessions.Count) сесій"
Write-Host "Claude Desktop після тестів: $($final.DesktopCount) процесів, $($final.Sessions.Count) сесій"

if ($baseline.DesktopCount -gt 0 -and $final.DesktopCount -eq 0) {
    Write-Host ""
    Write-Host "КРИТИЧНО: Claude Desktop не пережив тести!" -ForegroundColor Red
    exit 1
}

if ($script:failed -eq 0) {
    Write-Host ""
    Write-Host "Усі тести відмов пройдено. Claude Code не постраждав." -ForegroundColor Green
    exit 0
}

exit 1
