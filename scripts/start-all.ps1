# start-all.ps1 — піднімає всю систему однією командою.
#
# Запускає Relay, тунель до інтернету та Bridge, після чого перевіряє,
# що ланцюг справді працює. Адресу для застосунку друкує наприкінці.
#
# Тунель потрібен тому, що ноутбук і телефон стоять за NAT і не бачать
# один одного напряму. Обидва підключаються до Relay самі, тож жодних
# портів на роутері відкривати не треба.

[CmdletBinding()]
param(
    [ValidateSet("ngrok", "cloudflared", "none")]
    [string]$Tunnel = "ngrok",

    [int]$Port = 8787,

    # Сталий домен ngrok (безкоштовний план дає один).
    # Без нього адреса змінюватиметься за кожного запуску.
    [string]$Domain,

    [switch]$Stop
)

$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$relayDir = Join-Path $root "relay"
$bridgeExe = Join-Path $env:LOCALAPPDATA "ClaudeMonitor\claude-monitor-bridge.exe"
if (-not (Test-Path $bridgeExe)) { $bridgeExe = Join-Path $root "bridge\build\claude-monitor-bridge.exe" }

$stateDir = Join-Path $env:USERPROFILE ".claude-monitor\run"
if (-not (Test-Path $stateDir)) { New-Item -ItemType Directory -Force -Path $stateDir | Out-Null }

# Сталий домен, збережений під час першого налаштування.
#
# Безкоштовний акаунт ngrok отримує один «dev domain», закріплений за ним
# назавжди. Використання саме його означає, що адреса в застосунку
# лишається незмінною між перезапусками — інакше довелося б щоразу
# вводити нову.
$tunnelConfig = Join-Path $env:USERPROFILE ".claude-monitor\tunnel.json"
$saved = $null
if (Test-Path $tunnelConfig) {
    $saved = Get-Content $tunnelConfig -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $Domain -and $saved.domain) { $Domain = $saved.domain }
}

# Налаштування виконується один раз (scripts\setup.ps1): спосіб доступу,
# токен ngrok і адреса Relay. Без нього start-all не знає, куди підключатися.
$setupFile = Join-Path $env:USERPROFILE ".claude-monitor\setup.json"
if (-not $Stop -and -not (Test-Path $setupFile) -and -not $saved) {
    Write-Host "`nClaude Monitor ще не налаштовано на цьому комп'ютері." -ForegroundColor Yellow
    Write-Host "  Запустіть один раз:  .\scripts\setup.ps1`n"
    exit 3
}

# Пошук ngrok.
#
# У PATH winget кладе shim, який може вказувати на СТАРУ версію: команда
# `ngrok update` оновлює файл у каталозі Packages, а shim лишається
# попереднім. Стара версія шукає конфігурацію в іншому місці й повідомляє,
# що токена немає, хоча він збережений. Тому справжній виконуваний файл
# у Packages має пріоритет над тим, що в PATH.
$ngrokExe = $null

# Шлях зберігається після першого пошуку: рекурсивний обхід каталогу
# WinGet займає десятки секунд, і робити його за кожного запуску немає сенсу.
if ($saved -and $saved.ngrokPath -and (Test-Path $saved.ngrokPath)) {
    $ngrokExe = $saved.ngrokPath
}

if (-not $ngrokExe) {
    # Найімовірніше розташування — перевіряємо його першим, без обходу дерева.
    $likely = Join-Path $env:LOCALAPPDATA `
        "Microsoft\WinGet\Packages\Ngrok.Ngrok_Microsoft.Winget.Source_8wekyb3d8bbwe\ngrok.exe"
    if (Test-Path $likely) { $ngrokExe = $likely }
}

if (-not $ngrokExe) {
    $packaged = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter "ngrok.exe" `
        -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($packaged) { $ngrokExe = $packaged.FullName }
}

if (-not $ngrokExe) {
    $inPath = Get-Command ngrok -ErrorAction SilentlyContinue
    if ($inPath) { $ngrokExe = $inPath.Source }
}

# Запам'ятовуємо знайдений шлях, щоб наступний запуск був миттєвим.
if ($ngrokExe -and (-not $saved -or $saved.ngrokPath -ne $ngrokExe)) {
    $toSave = @{ provider = "ngrok"; domain = $Domain; ngrokPath = $ngrokExe }
    $toSave | ConvertTo-Json | Out-File $tunnelConfig -Encoding utf8 -NoNewline
}

function Write-Step { param($t) Write-Host "`n$t" -ForegroundColor Cyan }
function Write-Ok   { param($t) Write-Host "  $t" -ForegroundColor Green }
function Write-Bad  { param($t) Write-Host "  $t" -ForegroundColor Red }
function Write-Dim  { param($t) Write-Host "  $t" -ForegroundColor DarkGray }

# ── Зупинка ──────────────────────────────────────────────────────────────────

if ($Stop) {
    Write-Host "`nЗУПИНКА CLAUDE MONITOR" -ForegroundColor White
    foreach ($name in @("claude-monitor-bridge", "ngrok", "cloudflared")) {
        $procs = Get-Process $name -ErrorAction SilentlyContinue
        if ($procs) { $procs | Stop-Process -Force; Write-Ok "$name зупинено" }
    }
    # Relay — це node; вбиваємо лише той, що слухає наш порт.
    $conn = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
    if ($conn) {
        Stop-Process -Id $conn.OwningProcess -Force -ErrorAction SilentlyContinue
        Write-Ok "Relay зупинено"
    }
    Write-Host ""
    exit 0
}

Write-Host "`nЗАПУСК CLAUDE MONITOR" -ForegroundColor White

# ── 1. Relay ─────────────────────────────────────────────────────────────────

Write-Step "1. Relay"

$existing = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($existing) {
    Write-Dim "уже слухає порт $Port"
} else {
    if (-not (Test-Path (Join-Path $relayDir "node_modules\ws"))) {
        Write-Dim "встановлюю залежності…"
        Push-Location $relayDir
        npm install --no-audit --no-fund 2>&1 | Out-Null
        Pop-Location
    }

    Start-Process node -ArgumentList "src/index.js", "--mode", "tunnel", "--port", $Port `
        -WorkingDirectory $relayDir -WindowStyle Hidden `
        -RedirectStandardOutput "$stateDir\relay.log" -RedirectStandardError "$stateDir\relay-err.log" | Out-Null
    Start-Sleep -Seconds 3
}

try {
    $health = Invoke-RestMethod "http://127.0.0.1:$Port/health" -TimeoutSec 5
    Write-Ok "працює (протокол v$($health.protocol))"
} catch {
    Write-Bad "не відповідає — дивіться $stateDir\relay-err.log"
    exit 1
}

# ── 2. Тунель ────────────────────────────────────────────────────────────────

$publicUrl = $null

if ($Tunnel -eq "none") {
    Write-Step "2. Тунель пропущено"
    $ip = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -like "192.168.*" -or $_.IPAddress -like "10.*" } |
        Select-Object -First 1).IPAddress
    if ($ip) { $publicUrl = "ws://${ip}:$Port/ws"; Write-Dim "локальна адреса: $publicUrl" }

} elseif ($Tunnel -eq "ngrok") {
    Write-Step "2. Тунель ngrok"

    if (-not $ngrokExe) {
        Write-Bad "ngrok не встановлено: winget install ngrok.ngrok"
        exit 2
    }

    # Попередньої перевірки конфігурації тут навмисно немає.
    #
    # Спроби вгадати стан ngrok заздалегідь (шлях до ngrok.yml, розбір
    # виводу `config check`) давали хибний висновок «не налаштований» там,
    # де все було гаразд, і блокували запуск. Надійніше просто запустити
    # тунель: якщо з токеном щось не так, ngrok скаже це сам, і його
    # справжнє повідомлення буде показано нижче.

    Get-Process ngrok -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500

    # Токен передається через змінну середовища, а не через конфігурацію ngrok.
    #
    # Причина практична: ngrok, запущений із цього скрипта, не читав свій
    # ngrok.yml («cannot find the path specified»), хоча файл був на місці
    # й `config check` його бачив. Змінна NGROK_AUTHTOKEN — офіційно
    # підтримуваний спосіб, і він не залежить від того, де ngrok шукає
    # конфігурацію.
    #
    # Сам токен лежить на диску зашифрованим DPAPI: розшифрувати його може
    # лише цей обліковий запис Windows на цій машині. У командний рядок він
    # не потрапляє, тож не видно в переліку процесів.
    $tokenFile = Join-Path $env:USERPROFILE ".claude-monitor\ngrok-token.bin"
    if (Test-Path $tokenFile) {
        try {
            $secure = Get-Content $tokenFile -Raw | ConvertTo-SecureString
            $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
            $env:NGROK_AUTHTOKEN = [Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
        } catch {
            Write-Dim "не вдалося прочитати збережений токен"
        }
    }

    $ngrokArgs = @("http", "$Port", "--log", "stdout")
    if ($Domain) {
        $ngrokArgs += @("--url", "https://$Domain")
        Write-Dim "сталий домен: $Domain"
    }

    Start-Process $ngrokExe -ArgumentList $ngrokArgs -WindowStyle Hidden `
        -RedirectStandardOutput "$stateDir\ngrok.log" -RedirectStandardError "$stateDir\ngrok-err.log" | Out-Null

    # Адресу беремо з локального API ngrok — надійніше за розбір журналу.
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 1
        try {
            $api = Invoke-RestMethod "http://127.0.0.1:4040/api/tunnels" -TimeoutSec 3
            $https = $api.tunnels | Where-Object { $_.proto -eq "https" } | Select-Object -First 1
            if ($https) { $publicUrl = $https.public_url -replace '^https://', 'wss://'; $publicUrl += "/ws"; break }
        } catch { }
    }

    if ($publicUrl) {
        Write-Ok "піднято"
    } else {
        Write-Bad "не піднявся:"
        Get-Content "$stateDir\ngrok.log" -Tail 8 -ErrorAction SilentlyContinue | ForEach-Object { Write-Dim $_ }
        exit 1
    }

} elseif ($Tunnel -eq "cloudflared") {
    Write-Step "2. Тунель Cloudflare"
    Write-Dim "іменований тунель має бути налаштований заздалегідь (див. docs/relay.md)"
    Start-Process cloudflared -ArgumentList "tunnel", "run" -WindowStyle Hidden `
        -RedirectStandardOutput "$stateDir\cf.log" -RedirectStandardError "$stateDir\cf-err.log" | Out-Null
    Start-Sleep -Seconds 5
    Write-Dim "адресу візьміть із конфігурації тунелю"
}

# ── 3. Перевірка тунелю ──────────────────────────────────────────────────────

if ($publicUrl -and $publicUrl.StartsWith("wss://")) {
    Write-Step "3. Перевірка тунелю"
    $httpsUrl = ($publicUrl -replace '^wss://', 'https://') -replace '/ws$', '/health'
    try {
        $r = Invoke-RestMethod $httpsUrl -TimeoutSec 15
        Write-Ok "health через тунель: ok=$($r.ok)"
    } catch {
        Write-Bad "тунель не пропускає запити: $($_.Exception.Message.Split([Environment]::NewLine)[0])"
    }
}

# ── 4. Bridge ────────────────────────────────────────────────────────────────

Write-Step "4. Bridge"

if (-not (Test-Path $bridgeExe)) {
    Write-Bad "не знайдено: $bridgeExe"
    exit 2
}

if ($publicUrl) {
    $extra = @()
    if ($publicUrl.StartsWith("ws://")) { $extra += "--allow-insecure" }
    & $bridgeExe --set-relay $publicUrl @extra | Out-Null
    Write-Dim "адресу збережено"
}

Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

Start-Process $bridgeExe -ArgumentList "--log-level", "info" -WindowStyle Hidden `
    -RedirectStandardOutput "$stateDir\bridge-out.log" -RedirectStandardError "$stateDir\bridge-err.log" | Out-Null
Start-Sleep -Seconds 5

$bridgeLog = Join-Path $env:USERPROFILE ".claude-monitor\bridge.log"
$tail = (Get-Content $bridgeLog -Tail 15 -Encoding UTF8 -ErrorAction SilentlyContinue) -join "`n"

if ($tail -match "автентифіковано на Relay") {
    Write-Ok "підключений до Relay"
} elseif (Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue) {
    Write-Dim "працює, підключення триває"
} else {
    Write-Bad "не запустився"
}

# ── Підсумок ─────────────────────────────────────────────────────────────────

Write-Host "`n────────────────────────────────────────────────────────────" -ForegroundColor DarkGray
& $bridgeExe --status
Write-Host "────────────────────────────────────────────────────────────" -ForegroundColor DarkGray

if ($publicUrl) {
    Write-Host "`n  АДРЕСА ДЛЯ ЗАСТОСУНКУ:" -ForegroundColor White
    Write-Host "  $publicUrl`n" -ForegroundColor Yellow
    Write-Host "  Введіть її в застосунку: Налаштування -> Адреса Relay"
    Write-Host "  Якщо телефон ще не підключений: $bridgeExe --pair`n"
}

Write-Host "  Зупинити все:  .\scripts\start-all.ps1 -Stop`n" -ForegroundColor DarkGray
