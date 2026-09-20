# setup.ps1 — одноразове налаштування Claude Monitor на цьому комп'ютері.
#
# Навіщо: щоб кожен, хто взяв проєкт із GitHub, працював зі СВОЇМ тунелем
# і своїм Relay, а не з чужим. Запускається один раз; далі
# scripts\start-all.ps1 просто бере збережені налаштування.
#
# Що зберігається і де (нічого з цього не потрапляє в репозиторій):
#
#   %USERPROFILE%\.claude-monitor\setup.json        домен, порт, адреса Relay
#   %USERPROFILE%\.claude-monitor\tunnel.json       дані тунелю для start-all
#   %USERPROFILE%\.claude-monitor\ngrok-token.bin   токен ngrok, зашифрований DPAPI
#   %USERPROFILE%\.claude-monitor\bridge.json       адреса Relay для Bridge
#
# Безпека (Частина 3 Master Prompt):
#   • токен вводиться прихованим полем і НІКОЛИ не пишеться у відкритому
#     вигляді: на диску він зашифрований DPAPI, тобто читається лише цим
#     обліковим записом Windows і лише на цій машині;
#   • токен не потрапляє ні в командний рядок, ні в журнали, ні в змінні
#     середовища, які видно іншим процесам;
#   • доступ до файлів звужується до поточного користувача;
#   • ws:// (незашифроване з'єднання) приймається лише для локальної мережі.

[CmdletBinding()]
param(
    # Перепитати все наново, навіть якщо налаштування вже є.
    [switch]$Force,

    # Лише перевірити налаштування: 0 — усе гаразд, 1 — потрібне налаштування.
    [switch]$Check,

    [int]$Port = 8787
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$dataDir   = Join-Path $env:USERPROFILE ".claude-monitor"
$setupFile = Join-Path $dataDir "setup.json"
$tunnelFile = Join-Path $dataDir "tunnel.json"
$tokenFile = Join-Path $dataDir "ngrok-token.bin"

$root      = Split-Path -Parent $PSScriptRoot
$bridgeExe = Join-Path $env:LOCALAPPDATA "ClaudeMonitor\claude-monitor-bridge.exe"
if (-not (Test-Path $bridgeExe)) { $bridgeExe = Join-Path $root "bridge\build\claude-monitor-bridge.exe" }

function Write-Step { param($t) Write-Host "`n$t" -ForegroundColor Cyan }
function Write-Ok   { param($t) Write-Host "  $t" -ForegroundColor Green }
function Write-Bad  { param($t) Write-Host "  $t" -ForegroundColor Red }
function Write-Dim  { param($t) Write-Host "  $t" -ForegroundColor DarkGray }

# ── Перевірка адреси ─────────────────────────────────────────────────────────
#
# Ті самі правила, що й у застосунку: wss:// звідусіль, ws:// лише локально.
function Test-RelayUrl {
    param([string]$Url)

    if ([string]::IsNullOrWhiteSpace($Url)) { return "адресу не вказано" }
    if ($Url -notmatch '^(wss|ws)://[^\s/]+(/.*)?$') { return "адреса має виглядати як wss://домен/ws" }
    if ($Url -notmatch '/ws$') { return "адреса має закінчуватися на /ws" }

    if ($Url.StartsWith("ws://")) {
        # $host у PowerShell зайнятий самою оболонкою, тому власна назва.
        $hostName = ($Url -replace '^ws://', '') -split '/' | Select-Object -First 1
        $hostName = ($hostName -split ':')[0]
        $local = $hostName -eq "localhost" -or
                 $hostName -match '^127\.' -or $hostName -match '^10\.' -or
                 $hostName -match '^192\.168\.' -or $hostName -match '^172\.(1[6-9]|2[0-9]|3[01])\.'
        if (-not $local) { return "ws:// дозволене лише в локальній мережі; для інтернету потрібне wss://" }
    }
    return $null
}

function Read-Setup {
    if (-not (Test-Path $setupFile)) { return $null }
    try { return Get-Content $setupFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
}

function Test-Setup {
    $setup = Read-Setup
    if (-not $setup) { return "налаштування ще немає" }
    $problem = Test-RelayUrl $setup.relayUrl
    if ($problem) { return "збережена адреса непридатна: $problem" }
    if ($setup.tunnel -eq "ngrok" -and -not (Test-Path $tokenFile)) { return "немає збереженого токена ngrok" }
    return $null
}

# ── Режим перевірки ──────────────────────────────────────────────────────────

if ($Check) {
    $problem = Test-Setup
    if ($problem) {
        Write-Bad "Claude Monitor не налаштовано: $problem"
        Write-Dim "Запустіть: .\scripts\setup.ps1"
        exit 1
    }
    $setup = Read-Setup
    Write-Ok "налаштовано: $($setup.relayUrl)"
    exit 0
}

if (-not $Force) {
    $problem = Test-Setup
    if (-not $problem) {
        $setup = Read-Setup
        Write-Host "`nClaude Monitor уже налаштовано." -ForegroundColor White
        Write-Dim "адреса Relay: $($setup.relayUrl)"
        Write-Dim "щоб змінити: .\scripts\setup.ps1 -Force"
        Write-Host "`nДалі: .\scripts\start-all.ps1`n"
        exit 0
    }
    Write-Dim "потрібне налаштування: $problem"
}

Write-Host "`nНАЛАШТУВАННЯ CLAUDE MONITOR" -ForegroundColor White
Write-Dim "Дані зберігаються лише на цьому комп'ютері, у $dataDir"

if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Force -Path $dataDir | Out-Null }

# ── 1. Спосіб доступу з телефона ─────────────────────────────────────────────

Write-Step "1. Як телефон бачитиме ноутбук"
Write-Dim "1) ngrok — працює з будь-якої мережі, потрібен безкоштовний акаунт"
Write-Dim "2) лише локальна мережа — телефон і ноутбук в одному Wi-Fi"
Write-Dim "3) власний домен — у вас уже є HTTPS-адреса, що веде на Relay"

$choice = Read-Host "  Ваш вибір [1]"
if ([string]::IsNullOrWhiteSpace($choice)) { $choice = "1" }

$tunnel = "ngrok"
$domain = ""
$relayUrl = ""

switch ($choice) {
    "2" {
        $tunnel = "none"
        $ip = (Get-NetIPAddress -AddressFamily IPv4 |
               Where-Object { $_.IPAddress -like "192.168.*" -or $_.IPAddress -like "10.*" } |
               Select-Object -First 1).IPAddress
        if (-not $ip) { $ip = Read-Host "  Локальна IP-адреса ноутбука" }
        $relayUrl = "ws://${ip}:$Port/ws"
        Write-Dim "адреса для застосунку: $relayUrl"
    }
    "3" {
        $tunnel = "custom"
        $domain = Read-Host "  Ваш домен (наприклад monitor.example.com)"
        $relayUrl = "wss://$domain/ws"
    }
    default {
        $tunnel = "ngrok"
        Write-Dim "Токен: https://dashboard.ngrok.com/get-started/your-authtoken"
        Write-Dim "Сталий домен (безкоштовний план дає один): https://dashboard.ngrok.com/domains"

        $secure = Read-Host "  Токен ngrok (вводиться приховано)" -AsSecureString
        if (-not $secure -or $secure.Length -eq 0) {
            Write-Bad "токен не введено"
            exit 2
        }

        # На диск — лише у вигляді, зашифрованому DPAPI. Відкритого тексту
        # токена не бачить ні файл, ні журнал, ні перелік процесів.
        $secure | ConvertFrom-SecureString | Set-Content $tokenFile -Encoding ascii -NoNewline
        $secure.Dispose()

        # Доступ лише поточному користувачу.
        & icacls.exe $tokenFile /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
        Write-Ok "токен збережено зашифрованим (DPAPI)"

        $domain = Read-Host "  Сталий домен ngrok (Enter — щоразу нова адреса)"
        $domain = $domain.Trim()
        if ($domain) {
            $domain = $domain -replace '^https?://', '' -replace '/.*$', ''
            $relayUrl = "wss://$domain/ws"
        }
    }
}

# ── 2. Перевірка адреси ──────────────────────────────────────────────────────

if ($relayUrl) {
    $problem = Test-RelayUrl $relayUrl
    if ($problem) {
        Write-Bad "адреса непридатна: $problem"
        exit 2
    }
    Write-Ok "адреса Relay: $relayUrl"
} else {
    Write-Dim "адреса з'явиться після першого запуску start-all.ps1 (ngrok видасть її сам)"
}

# ── 3. Збереження ────────────────────────────────────────────────────────────

Write-Step "2. Збереження налаштувань"

$saved = Get-Content $tunnelFile -Raw -Encoding UTF8 -ErrorAction SilentlyContinue |
         ConvertFrom-Json -ErrorAction SilentlyContinue
$tunnelData = [ordered]@{
    provider  = $tunnel
    domain    = $domain
    ngrokPath = if ($saved -and $saved.ngrokPath) { $saved.ngrokPath } else { $null }
}
$tunnelData | ConvertTo-Json | Set-Content $tunnelFile -Encoding utf8 -NoNewline

$setupData = [ordered]@{
    version   = 1
    tunnel    = $tunnel
    domain    = $domain
    port      = $Port
    relayUrl  = $relayUrl
    createdAt = (Get-Date).ToString("o")
}
$setupData | ConvertTo-Json | Set-Content $setupFile -Encoding utf8 -NoNewline
& icacls.exe $setupFile /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
Write-Ok "налаштування збережено в $dataDir"

# Папка вузлів — розширень, які показують у застосунку свої картки
# (docs/nodes.md). Порожня папка нікому не заважає, але позбавляє
# зайвого кроку тих, хто захоче додати вузол.
$nodesDir = Join-Path $dataDir "nodes"
if (-not (Test-Path $nodesDir)) {
    New-Item -ItemType Directory -Force -Path $nodesDir | Out-Null
}

# ── 4. Адреса для Bridge ─────────────────────────────────────────────────────

if ($relayUrl -and (Test-Path $bridgeExe)) {
    $extra = @()
    if ($relayUrl.StartsWith("ws://")) { $extra += "--allow-insecure" }
    & $bridgeExe --set-relay $relayUrl @extra | Out-Null
    Write-Ok "адресу передано Bridge"
} elseif (-not (Test-Path $bridgeExe)) {
    Write-Dim "Bridge ще не зібрано — адреса збережеться під час першого запуску start-all.ps1"
}

# ── 5. Що далі ───────────────────────────────────────────────────────────────

Write-Host "`n────────────────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "  ГОТОВО. Далі:" -ForegroundColor White
Write-Host ""
Write-Host "  1. Запустіть систему:      .\scripts\start-all.ps1"
Write-Host "  2. Підключіть телефон:     `"$bridgeExe`" --pair" -ForegroundColor Yellow
Write-Host "     У застосунку введіть адресу Relay і код із вікна."
Write-Host "  3. Вузли (необов'язково):  .\scripts\install-node.ps1 .\nodes\system-info"
Write-Host ""
Write-Dim "Телефонів можна підключити до п'яти. Повторне налаштування: setup.ps1 -Force"
Write-Host ""
