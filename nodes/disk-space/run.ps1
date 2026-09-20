# disk-space — скільки місця лишилось на дисках.

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false

# DriveType=3 — лише внутрішні диски: флешки та мережеві теки не цікавлять.
$disks = Get-CimInstance Win32_LogicalDisk -Filter "DriveType=3" |
         Where-Object { $_.Size -gt 0 }

$lines = @()
$low = $false

foreach ($disk in $disks) {
    $freeGb  = [math]::Round($disk.FreeSpace / 1GB, 1)
    $totalGb = [math]::Round($disk.Size / 1GB, 1)
    $percent = [int](100 * $disk.FreeSpace / $disk.Size)

    if ($percent -lt 10) { $low = $true }

    $lines += [ordered]@{
        label = $disk.DeviceID
        value = "$freeGb / $totalGb ГБ ($percent %)"
    }
}

[ordered]@{
    status = if ($low) { "warn" } else { "ok" }
    lines  = $lines
    text   = if ($low) { "Менш ніж 10 % вільного місця." } else { "" }
} | ConvertTo-Json -Depth 4 -Compress
