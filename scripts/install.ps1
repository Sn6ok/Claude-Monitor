# install.ps1 — встановлення Claude Monitor на ноутбуці.
#
# Скрипт нічого не робить мовчки: кожна зміна у ваших файлах показується
# й потребує підтвердження. Найважливіше — правка
# %USERPROFILE%\.claude\settings.json, куди додаються хуки.
#
# Адміністративні права НЕ потрібні: усе відбувається у вашому профілі.

[CmdletBinding()]
param(
    [string]$InstallDir = "$env:LOCALAPPDATA\ClaudeMonitor",
    [string]$RelayUrl,
    [switch]$Uninstall,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$sourceExe = Join-Path $root "bridge\build\claude-monitor-bridge.exe"
$targetExe = Join-Path $InstallDir "claude-monitor-bridge.exe"
$settingsPath = Join-Path $env:USERPROFILE ".claude\settings.json"

function Write-Step { param([string]$Text) Write-Host "`n$Text" -ForegroundColor Cyan }
function Write-Ok   { param([string]$Text) Write-Host "  $Text" -ForegroundColor Green }
function Write-Warn { param([string]$Text) Write-Host "  $Text" -ForegroundColor Yellow }
function Write-Info { param([string]$Text) Write-Host "  $Text" -ForegroundColor Gray }

function Confirm-Action {
    param([string]$Question)
    if ($Force) { return $true }
    $answer = Read-Host "  $Question [т/н]"
    return $answer -match '^[тty]'
}

# ── Видалення ────────────────────────────────────────────────────────────────

if ($Uninstall) {
    Write-Host "`nВИДАЛЕННЯ CLAUDE MONITOR" -ForegroundColor White

    Write-Step "1. Зупинка Bridge"
    $running = Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue
    if ($running) {
        $running | Stop-Process -Force
        Write-Ok "зупинено"
    } else {
        Write-Info "не працював"
    }

    Write-Step "2. Хуки в settings.json"
    if (Test-Path $settingsPath) {
        $settings = Get-Content $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $hasHooks = $settings.PSObject.Properties.Name -contains "hooks"

        if ($hasHooks) {
            Write-Warn "хуки треба прибрати вручну з $settingsPath"
            Write-Info "видаліть записи, що згадують claude-monitor-bridge"
        } else {
            Write-Info "хуків немає"
        }
    }

    Write-Step "3. Дані Bridge"
    $dataDir = Join-Path $env:USERPROFILE ".claude-monitor"
    if (Test-Path $dataDir) {
        Write-Info "каталог: $dataDir"
        Write-Info "містить ключі, конфігурацію та журнал"
        if (Confirm-Action "Видалити?") {
            Remove-Item $dataDir -Recurse -Force
            Write-Ok "видалено"
        } else {
            Write-Info "залишено"
        }
    }

    Write-Step "4. Файли програми"
    if (Test-Path $InstallDir) {
        if (Confirm-Action "Видалити $InstallDir ?") {
            Remove-Item $InstallDir -Recurse -Force
            Write-Ok "видалено"
        }
    }

    Write-Host "`nГотово. Каталог ~/.claude не змінювався: він належить Claude Code.`n"
    exit 0
}

# ── Встановлення ─────────────────────────────────────────────────────────────

Write-Host "`nВСТАНОВЛЕННЯ CLAUDE MONITOR" -ForegroundColor White

# 1. Перевірка збірки
Write-Step "1. Перевірка файлів"
if (-not (Test-Path $sourceExe)) {
    Write-Host "  Bridge не зібрано: $sourceExe" -ForegroundColor Red
    Write-Host "  Спершу виконайте:" -ForegroundColor Red
    Write-Host "    cd bridge" -ForegroundColor Red
    Write-Host "    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release" -ForegroundColor Red
    Write-Host "    cmake --build build`n" -ForegroundColor Red
    exit 2
}
Write-Ok "Bridge знайдено ($([Math]::Round((Get-Item $sourceExe).Length / 1KB)) КБ)"

# 2. Claude Desktop
Write-Step "2. Claude Desktop"
$claude = Get-Process claude -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like "*WindowsApps*Claude*" -or $_.Path -like "*AnthropicClaude*" }
if ($claude) {
    Write-Ok "працює"
} else {
    Write-Warn "не запущено — це нормально, перевіримо пізніше"
}

# 3. Копіювання
Write-Step "3. Копіювання програми"
if (-not (Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}

$running = Get-Process claude-monitor-bridge -ErrorAction SilentlyContinue
if ($running) {
    Write-Info "зупиняю попередню версію"
    $running | Stop-Process -Force
    Start-Sleep -Seconds 1
}

Copy-Item $sourceExe $targetExe -Force
Write-Ok "встановлено: $targetExe"

# 4. Адреса Relay
Write-Step "4. Адреса сервера"
if (-not $RelayUrl) {
    Write-Info "Relay — це сервер, через який телефон знаходить ноутбук."
    Write-Info "Найпростіше підняти його через Cloudflare Tunnel (див. docs/relay.md)."
    Write-Host ""
    $RelayUrl = Read-Host "  Адреса (наприклад wss://monitor.example.com/ws), або Enter щоб пропустити"
}

if ($RelayUrl) {
    if ($RelayUrl -notmatch '^wss://' -and $RelayUrl -notmatch '^ws://') {
        Write-Warn "адреса має починатися з wss:// — пропускаю"
    } else {
        $extra = @()
        if ($RelayUrl -match '^ws://') {
            Write-Warn "адреса без шифрування: припустимо лише для локальних тестів"
            $extra += "--allow-insecure"
        }
        & $targetExe --set-relay $RelayUrl @extra | Out-Null
        Write-Ok "збережено"
    }
} else {
    Write-Info "пропущено — задайте пізніше: claude-monitor-bridge.exe --set-relay URL"
}

# 5. Хуки — найважливіший крок
Write-Step "5. Запуск разом із Claude Code"
Write-Info "Bridge має стартувати, коли починається сесія Claude Code,"
Write-Info "і НЕ стартувати разом із Windows. Для цього в файл"
Write-Info "  $settingsPath"
Write-Info "додаються три хуки: SessionStart, SessionEnd, Notification."
Write-Host ""

$escapedPath = $targetExe -replace '\\', '\\\\'

$hooksToAdd = @{
    SessionStart = "`"$escapedPath`" --hook session-start"
    SessionEnd   = "`"$escapedPath`" --hook session-end"
    Notification = "`"$escapedPath`" --hook notification"
}

if (Confirm-Action "Додати хуки автоматично?") {

    # Резервна копія: користувач має мати змогу повернути як було.
    if (Test-Path $settingsPath) {
        $backup = "$settingsPath.backup-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        Copy-Item $settingsPath $backup
        Write-Ok "резервна копія: $backup"
        $settings = Get-Content $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } else {
        $settingsDir = Split-Path -Parent $settingsPath
        if (-not (Test-Path $settingsDir)) {
            New-Item -ItemType Directory -Force -Path $settingsDir | Out-Null
        }
        $settings = [PSCustomObject]@{}
    }

    if (-not ($settings.PSObject.Properties.Name -contains "hooks")) {
        $settings | Add-Member -NotePropertyName "hooks" -NotePropertyValue ([PSCustomObject]@{})
    }

    $added = 0
    foreach ($event in $hooksToAdd.Keys) {
        $command = $hooksToAdd[$event]

        $entry = [PSCustomObject]@{
            hooks = @([PSCustomObject]@{ type = "command"; command = $command })
        }

        $existing = $settings.hooks.PSObject.Properties.Name -contains $event
        if ($existing) {
            # Не затираємо чужі хуки: додаємо свій до наявних, якщо його ще немає.
            $current = @($settings.hooks.$event)
            $alreadyThere = $current | Where-Object {
                ($_ | ConvertTo-Json -Depth 5) -like "*claude-monitor-bridge*"
            }
            if ($alreadyThere) {
                Write-Info "$event — уже налаштовано"
                continue
            }
            $settings.hooks.$event = @($current + $entry)
        } else {
            $settings.hooks | Add-Member -NotePropertyName $event -NotePropertyValue @($entry)
        }
        $added++
    }

    if ($added -gt 0) {
        $json = $settings | ConvertTo-Json -Depth 10
        [System.IO.File]::WriteAllText($settingsPath, $json, (New-Object System.Text.UTF8Encoding($false)))
        Write-Ok "додано хуків: $added"
    } else {
        Write-Info "усі хуки вже були на місці"
    }

} else {
    Write-Info "пропущено. Показати фрагмент для ручного додавання:"
    Write-Host ""
    & $targetExe --install-hooks
}

# 6. Перевірка
Write-Step "6. Перевірка"
& $targetExe --status

# 7. Що далі
Write-Host "`n────────────────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host "ЩО ДАЛІ" -ForegroundColor White
Write-Host "────────────────────────────────────────────────────────────" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  1. Запустіть Relay (див. docs/relay.md)"
Write-Host "  2. Встановіть APK на телефон"
Write-Host "  3. Підключіть телефон:"
Write-Host "       `"$targetExe`" --pair" -ForegroundColor Yellow
Write-Host ""
Write-Host "  Bridge стартуватиме сам разом із сесіями Claude Code."
Write-Host ""
