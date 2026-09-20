# autostart.ps1 — запуск Claude Monitor разом із входом у Windows.
#
# За замовчуванням Claude Monitor НЕ стартує разом із системою: Bridge
# піднімає хук SessionStart, коли ви починаєте сесію Claude Code
# (docs/windows-bridge.md §1). Цей скрипт — свідомий виняток для тих, хто
# хоче, щоб Relay і тунель були готові одразу після входу в систему.
#
# Що саме автозапускається: scripts\start-all.ps1 — тобто Relay, тунель
# і Bridge. Якщо Claude Desktop не запущено, Bridge сам завершиться за
# кілька хвилин, а Relay із тунелем лишаться напоготові. Коли ви почнете
# сесію Claude Code, хук підніме Bridge знову.
#
# Прав адміністратора не потрібно: усе робиться у вашому обліковому записі.
#
#   .\scripts\autostart.ps1 -Enable      увімкнути
#   .\scripts\autostart.ps1 -Disable     вимкнути
#   .\scripts\autostart.ps1 -Status      перевірити стан

[CmdletBinding()]
param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$Status
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$taskName = "ClaudeMonitor"
$runKey   = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$runName  = "ClaudeMonitor"

$root      = Split-Path -Parent $PSScriptRoot
$startAll  = Join-Path $root "scripts\start-all.ps1"
$command   = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$startAll`""

function Write-Ok   { param($t) Write-Host "  $t" -ForegroundColor Green }
function Write-Bad  { param($t) Write-Host "  $t" -ForegroundColor Red }
function Write-Dim  { param($t) Write-Host "  $t" -ForegroundColor DarkGray }

function Get-TaskState {
    try { return Get-ScheduledTask -TaskName $taskName -ErrorAction Stop } catch { return $null }
}

function Get-RunEntry {
    try { return (Get-ItemProperty -Path $runKey -Name $runName -ErrorAction Stop).$runName } catch { return $null }
}

# ── Стан ─────────────────────────────────────────────────────────────────────

if ($Status -or (-not $Enable -and -not $Disable)) {
    Write-Host "`nАВТОЗАПУСК CLAUDE MONITOR" -ForegroundColor White

    $task = Get-TaskState
    $run = Get-RunEntry

    if ($task) {
        Write-Ok "увімкнено через Планувальник завдань (завдання «$taskName»)"
        Write-Dim "стан: $($task.State)"
    } elseif ($run) {
        Write-Ok "увімкнено через автозапуск користувача (реєстр HKCU\...\Run)"
        Write-Dim $run
    } else {
        Write-Dim "вимкнено — Claude Monitor запускається лише з сесією Claude Code"
        Write-Dim "увімкнути: .\scripts\autostart.ps1 -Enable"
    }

    if (-not (Test-Path $startAll)) {
        Write-Bad "не знайдено $startAll"
    }
    Write-Host ""
    exit 0
}

# ── Вимкнення ────────────────────────────────────────────────────────────────

if ($Disable) {
    Write-Host "`nВИМКНЕННЯ АВТОЗАПУСКУ" -ForegroundColor White
    $removed = $false

    if (Get-TaskState) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
        Write-Ok "завдання планувальника видалено"
        $removed = $true
    }
    if (Get-RunEntry) {
        Remove-ItemProperty -Path $runKey -Name $runName
        Write-Ok "запис автозапуску видалено"
        $removed = $true
    }
    if (-not $removed) { Write-Dim "автозапуск і так вимкнений" }

    Write-Dim "вже запущені Relay, тунель і Bridge це не зупиняє"
    Write-Dim "зупинити зараз: .\scripts\start-all.ps1 -Stop"
    Write-Host ""
    exit 0
}

# ── Увімкнення ───────────────────────────────────────────────────────────────

Write-Host "`nУВІМКНЕННЯ АВТОЗАПУСКУ" -ForegroundColor White

if (-not (Test-Path $startAll)) {
    Write-Bad "не знайдено $startAll"
    exit 2
}

# Налаштування має бути зроблене заздалегідь: інакше автозапуск щоразу
# впирався б у запит даних, якого ніхто не побачить.
$setupFile = Join-Path $env:USERPROFILE ".claude-monitor\setup.json"
$tunnelFile = Join-Path $env:USERPROFILE ".claude-monitor\tunnel.json"
if (-not (Test-Path $setupFile) -and -not (Test-Path $tunnelFile)) {
    Write-Bad "спершу виконайте одноразове налаштування: .\scripts\setup.ps1"
    exit 3
}

# Планувальник кращий за реєстр: вікно не блимає, є повтор при збої
# і запуск не залежить від того, скільки триває вхід у систему.
$viaTask = $false
try {
    $action = New-ScheduledTaskAction -Execute "powershell.exe" `
        -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$startAll`""
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
        -Settings $settings -Principal $principal -Force `
        -Description "Claude Monitor: Relay, тунель і Bridge після входу в систему" | Out-Null

    $viaTask = $true
    Write-Ok "додано завдання планувальника «$taskName» (вхід у систему)"
} catch {
    Write-Dim "планувальник недоступний: $($_.Exception.Message.Split([Environment]::NewLine)[0])"
}

if (-not $viaTask) {
    # Запасний шлях — автозапуск користувача. Працює завжди й без
    # адміністративних прав; ціна — коротке блимання вікна PowerShell.
    if (-not (Test-Path $runKey)) { New-Item -Path $runKey -Force | Out-Null }
    Set-ItemProperty -Path $runKey -Name $runName -Value $command
    Write-Ok "додано запис автозапуску користувача"
    Write-Dim "під час входу коротко блимне вікно PowerShell — це нормально"
}

Write-Host ""
Write-Dim "перевірити:  .\scripts\autostart.ps1 -Status"
Write-Dim "вимкнути:    .\scripts\autostart.ps1 -Disable"
Write-Host ""
