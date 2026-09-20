# Картка вузла Claude Click: чи готова програма і які були останні кліки.
#
# Журнал пише сам click.py у цю ж папку. Якщо кліків ще не було — так і кажемо,
# а не вигадуємо порожню картку.

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false

$base = Split-Path -Parent $MyInvocation.MyCommand.Path
$log = Join-Path $base "clicks.log"

$lines = @()
$status = "ok"
$text = ""

# Pillow потрібен лише для знімка екрана. Без нього клік працює, але
# цілитись немає куди — і про це має бути видно з телефона.
$pillow = $false
try {
    & python -c "import PIL" 2>$null
    $pillow = ($LASTEXITCODE -eq 0)
} catch {
    $pillow = $false
}

if (-not $pillow) {
    $status = "warn"
    $text = "Немає Pillow: pip install pillow"
}

if (Test-Path $log) {
    $entries = @(Get-Content $log -Encoding UTF8 -Tail 200 | Where-Object { $_ -match "\t" })

    if ($entries.Count -gt 0) {
        $today = (Get-Date).ToString("yyyy-MM-dd")
        $todayCount = @($entries | Where-Object { $_.StartsWith($today) }).Count

        $last = $entries[-1].Split("`t")
        $when = $null
        try { $when = [datetime]::Parse($last[0]) } catch { $when = $null }

        $label = if ($last.Count -gt 1 -and $last[1]) { $last[1] } else { "точка $($last[2])" }

        $lines += [ordered]@{ label = "Сьогодні"; value = "$todayCount кліків" }
        $lines += [ordered]@{ label = "Останній"; value = $label }

        if ($when) {
            $ago = (Get-Date) - $when
            $agoText = if ($ago.TotalMinutes -lt 1) { "щойно" }
                       elseif ($ago.TotalHours -lt 1) { "{0} хв тому" -f [int]$ago.TotalMinutes }
                       elseif ($ago.TotalDays -lt 1) { "{0} год тому" -f [int]$ago.TotalHours }
                       else { "{0} д тому" -f [int]$ago.TotalDays }
            $lines += [ordered]@{ label = "Коли"; value = $agoText }
        }
    }
}

if ($lines.Count -eq 0) {
    $lines += [ordered]@{ label = "Кліків"; value = "ще не було" }
}

[ordered]@{
    status = $status
    lines  = $lines
    text   = $text
} | ConvertTo-Json -Depth 4 -Compress
