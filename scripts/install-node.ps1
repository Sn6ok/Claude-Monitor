<#
.SYNOPSIS
    Встановлює вузол (node) — розширення Claude Monitor.

.DESCRIPTION
    Копіює каталог вузла в %USERPROFILE%\.claude-monitor\nodes.
    Bridge перечитує папку сам, щонайбільше за 10 секунд: перезапускати
    нічого не треба.

    Вузол — це чужий код, який виконається на вашому комп'ютері з вашими
    правами. Скрипт показує, що саме запускатиметься, і питає згоди.

.PARAMETER Path
    Каталог вузла (у ньому має бути node.json).

.PARAMETER List
    Показати встановлені вузли.

.PARAMETER Remove
    Прибрати встановлений вузол за його іменем.

.PARAMETER Force
    Перезаписати вже встановлений вузол без запитань.

.EXAMPLE
    .\scripts\install-node.ps1 .\nodes\system-info
    .\scripts\install-node.ps1 -List
    .\scripts\install-node.ps1 -Remove system-info
#>

[CmdletBinding(DefaultParameterSetName = "Install")]
param(
    [Parameter(ParameterSetName = "Install", Position = 0, Mandatory = $true)]
    [string]$Path,

    [Parameter(ParameterSetName = "List", Mandatory = $true)]
    [switch]$List,

    [Parameter(ParameterSetName = "Remove", Mandatory = $true)]
    [string]$Remove,

    [Parameter(ParameterSetName = "Install")]
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$nodesDir = Join-Path $env:USERPROFILE ".claude-monitor\nodes"

function Write-Ok   { param($m) Write-Host "  ✓ $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "  ! $m" -ForegroundColor Yellow }
function Write-Dim  { param($m) Write-Host "  $m" -ForegroundColor DarkGray }

if (-not (Test-Path $nodesDir)) {
    New-Item -ItemType Directory -Force -Path $nodesDir | Out-Null
}

# ── Перелік ──────────────────────────────────────────────────────────────────

if ($List) {
    $installed = Get-ChildItem $nodesDir -Directory -ErrorAction SilentlyContinue
    if (-not $installed) {
        Write-Dim "Вузлів немає. Приклади: .\nodes\"
        exit 0
    }

    Write-Host "`nВстановлені вузли ($nodesDir):`n" -ForegroundColor White
    foreach ($dir in $installed) {
        $manifestFile = Join-Path $dir.FullName "node.json"
        $manifest = if (Test-Path $manifestFile) {
            Get-Content $manifestFile -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction SilentlyContinue
        } else { $null }

        if ($manifest) {
            $state = if ($manifest.enabled -eq $false) { "вимкнено" } else { "увімкнено" }
            Write-Host ("  {0,-20} {1,-28} {2}" -f $dir.Name, $manifest.name, $state)
        } else {
            Write-Warn "$($dir.Name) — немає придатного node.json"
        }
    }
    Write-Host ""
    exit 0
}

# ── Видалення ────────────────────────────────────────────────────────────────

if ($Remove) {
    # Ім'я, а не шлях: видаляємо лише всередині папки вузлів і ніде більше.
    if ($Remove -match '[\\/:*?"<>|]') {
        Write-Warn "Вкажіть саме ім'я вузла, наприклад: -Remove system-info"
        exit 1
    }

    $target = Join-Path $nodesDir $Remove
    if (-not (Test-Path $target)) {
        Write-Warn "Вузла «$Remove» не встановлено"
        exit 1
    }

    Remove-Item $target -Recurse -Force
    Write-Ok "вузол «$Remove» прибрано"
    exit 0
}

# ── Встановлення ─────────────────────────────────────────────────────────────

if (-not (Test-Path $Path)) {
    Write-Warn "Каталогу не знайдено: $Path"
    exit 1
}

$source = (Resolve-Path $Path).Path
$manifestFile = Join-Path $source "node.json"

if (-not (Test-Path $manifestFile)) {
    Write-Warn "У каталозі немає node.json — це не вузол"
    exit 1
}

$manifest = Get-Content $manifestFile -Raw -Encoding UTF8 | ConvertFrom-Json
$name = if ($manifest.id) { $manifest.id } else { Split-Path $source -Leaf }

if ($name -match '[^A-Za-z0-9._-]') {
    Write-Warn "Недопустиме ім'я вузла: $name"
    exit 1
}

if (-not $manifest.run) {
    Write-Warn "У node.json не вказано «run» — нічого запускати"
    exit 1
}

Write-Host ""
Write-Host "  Вузол:    $($manifest.name)" -ForegroundColor White
if ($manifest.description) { Write-Dim "  $($manifest.description)" }
Write-Host "  Запускає: $($manifest.run)" -ForegroundColor Yellow
Write-Host "  Кожні:    $(if ($manifest.intervalSec) { $manifest.intervalSec } else { 30 }) с"
Write-Host ""
Write-Dim "  Це чужий код: він виконається з вашими правами."
Write-Dim "  Переглянути: $(Join-Path $source $manifest.run)"
Write-Host ""

$target = Join-Path $nodesDir $name

if ((Test-Path $target) -and -not $Force) {
    Write-Warn "Вузол «$name» уже встановлено. Перезаписати: -Force"
    exit 1
}

if (-not $Force) {
    $answer = Read-Host "  Встановити? (так/ні)"
    if ($answer -notmatch '^(т|y)') {
        Write-Dim "Скасовано."
        exit 0
    }
}

if (Test-Path $target) { Remove-Item $target -Recurse -Force }
Copy-Item $source $target -Recurse -Force

Write-Ok "встановлено в $target"
Write-Dim "Bridge підхопить вузол упродовж 10 секунд — картка з'явиться в застосунку."
Write-Host ""
