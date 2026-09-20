# template — заготовка власного вузла.
#
# Як зробити свій:
#   1. Скопіюйте цей каталог під іншим іменем у nodes\.
#   2. Змініть "id", "name" та "enabled": true в node.json.
#   3. Напишіть тут свій збір даних.
#   4. .\scripts\install-node.ps1 .\nodes\ваш-вузол
#
# Єдина вимога: надрукувати в stdout один об'єкт JSON і вийти з кодом 0.
#
#   status — "ok" | "warn" | "error" (колір крапки в застосунку)
#   lines  — до 12 пар «підпис — значення»
#   text   — довільний текст під рядками (необов'язковий)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false

[ordered]@{
    status = "ok"
    lines  = @(
        [ordered]@{ label = "Час"; value = (Get-Date).ToString("HH:mm:ss") }
    )
    text   = "Це заготовка. Замініть її своїми даними."
} | ConvertTo-Json -Depth 4 -Compress
