# system-info — навантаження ноутбука однією карткою.
#
# Друкує один об'єкт JSON у stdout і завершується. Нічого не зберігає,
# нікуди не звертається по мережі.

$ErrorActionPreference = "Stop"

# Bridge читає stdout як UTF-8. Без цього рядка PowerShell 5.1 віддав би
# кирилицю кодуванням консолі, і підписи перетворилися б на кракозябри.
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false

$cpu = [math]::Round(
    (Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average, 0)

$os = Get-CimInstance Win32_OperatingSystem
# Пам'ять CIM віддає в кілобайтах, тому ділення на 1MB дає гігабайти.
$freeGb  = [math]::Round($os.FreePhysicalMemory / 1MB, 1)
$totalGb = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)

$uptime = (Get-Date) - $os.LastBootUpTime
$uptimeText = if ($uptime.TotalDays -ge 1) {
    "{0} д {1} год" -f [int]$uptime.TotalDays, $uptime.Hours
} else {
    "{0} год {1} хв" -f [int]$uptime.TotalHours, $uptime.Minutes
}

# Жовтий стан — коли ноутбуку вже важко: про це варто знати з телефона.
$status = if ($cpu -ge 90 -or $freeGb -lt 1.0) { "warn" } else { "ok" }

[ordered]@{
    status = $status
    lines  = @(
        [ordered]@{ label = "Процесор";      value = "$cpu %" }
        [ordered]@{ label = "Вільна пам'ять"; value = "$freeGb / $totalGb ГБ" }
        [ordered]@{ label = "Працює";         value = $uptimeText }
    )
} | ConvertTo-Json -Depth 4 -Compress
