# Дослідження середовища — фактичні результати

> Цей документ фіксує **перевірені факти**, а не припущення.
> Master Prompt (Частина 5 §5, §12; Частина 6 §2) прямо забороняє вигадувати API
> та вимагає перевіряти реальну поведінку поточної версії Claude Desktop.

Дата дослідження: 2026-09-09
Машина: Windows 11 Pro 10.0.26200, x64

---

## 1. Claude Desktop

| Параметр | Значення |
|---|---|
| Шлях | `C:\Program Files\WindowsApps\Claude_1.49585.0.0_x64__pzs8sxrjxfjjc\app\Claude.exe` |
| Тип встановлення | MSIX (Microsoft Store), каталог захищений від запису |
| Версія застосунку | 1.49585.0 |
| Runtime | Electron 44.2.0 (Chromium multi-process) |
| Кількість процесів | 16 (1 root + renderer/gpu/network/utility/crashpad) |
| User data | `C:\Users\<user>\AppData\Roaming\Claude` |

### Структура процесів

Усі процеси мають імʼя `claude.exe`, тому **імʼя процесу не є надійним ідентифікатором**
(підтверджує вимогу Частини 5 §9).

```text
Claude.exe (root, PID 15544)            <- ParentProcessId вказує назовні
├── Claude.exe --type=gpu-process
├── Claude.exe --type=renderer          <- вікна застосунку
├── Claude.exe --type=utility ...       <- network, audio, video, node services
├── Claude.exe --type=crashpad-handler
└── claude.exe (claude-code\2.1.260\)   <- ОКРЕМИЙ бінарник, це Claude Code
```

**Надійне правило визначення root-процесу Claude Desktop:**

```text
ExecutablePath містить "\WindowsApps\Claude_"  AND  CommandLine НЕ містить "--type="
```

Це стабільно між версіями, бо `--type=` — стандартна ознака дочірніх процесів Chromium.

### Наслідки для проєкту

* Модифікувати Claude Desktop **неможливо і не потрібно** — MSIX-каталог захищений.
  Це автоматично виключає DLL injection і патчинг (Частина 5 §5, §11).
* Привʼязка життєвого циклу до root-процесу можлива через `OpenProcess` +
  `WaitForSingleObject` — це **event-driven, 0% CPU** в очікуванні (Частина 5 §7, §31).

---

## 2. Claude Code

**Ключовий факт: Claude Code — це окремий виконуваний файл, а не той самий процес,
що й Claude Desktop.**

| Параметр | Значення |
|---|---|
| Шлях | `C:\Users\<user>\AppData\Roaming\Claude\claude-code\2.1.260\claude.exe` |
| Версія | 2.1.260 |
| Розмір | 217 MB (Node.js Single Executable Application) |
| Спосіб запуску | дочірній процес Claude Desktop |

Реальний командний рядок (скорочено):

```text
claude.exe --output-format stream-json --verbose --input-format stream-json
           --model claude-opus-5 --permission-prompt-tool stdio
           --setting-sources=user,project,local
           --permission-mode auto --include-partial-messages
```

### Наслідки

1. `--setting-sources=user,project,local` — **hooks з `~/.claude/settings.json` активні
   в Claude Desktop**. Це офіційний, документований механізм інтеграції = Priority 1
   за класифікацією Частини 5 §11.
2. Одночасно спостерігалося **3 активні сесії Claude Code**. Отже protocol
   зобовʼязаний мати `session_id` (Частина 5 §38, §39).
3. `Claude Desktop RUNNING` != `Claude Code WORKING` — це різні сутності,
   як і вимагає Частина 5 §10.

---

## 3. Джерела даних — що реально доступно

Перевірено три джерела. Жодне з них не потребує reverse engineering,
читання памʼяті чи інжекту.

### 3.1 Реєстр живих сесій — `~/.claude/sessions/<PID>.json`

Реальний вміст (значення скорочені):

```json
{
  "pid": 15504,
  "sessionId": "fa676105-0796-4700-a4ca-2014ea84e18e",
  "cwd": "D:\\Wall\\For Hacker\\Claude Monito",
  "startedAt": 1788953990322,
  "version": "2.1.260",
  "kind": "interactive",
  "entrypoint": "claude-desktop",
  "name": "claude-monito-47"
}
```

Дає: перелік активних сесій, їхні PID, робочі каталоги, час старту, людську назву.
Вартість читання: **нульова** для Claude Code — це звичайні файли.

> **Примітка про безпеку.** Файл також містить поле `messagingSocketPath`
> (named pipe внутрішнього протоколу Claude Code). Bridge **не використовує** цей
> канал: це недокументований внутрішній протокол, і Частина 2 §3 та Частина 5 §11
> вимагають уникати таких механізмів за наявності альтернативи. Альтернатива є.

### 3.2 Потік подій — транскрипт сесії

Шлях: `~/.claude/projects/<slug-робочого-каталогу>/<sessionId>.jsonl`

Формат: JSON Lines, дописується в реальному часі (append-only).
Шлях до цього файлу офіційно передається хукам у полі `transcript_path`,
тобто це **документована частина інтерфейсу**, а не внутрішня деталь.

Перевірені типи записів:

| `type` | Зміст |
|---|---|
| `assistant` | `message.content[]` з елементами `text`, `thinking`, `tool_use` |
| `user` | `message.content[]` з `tool_result`, плюс `toolUseResult` |
| `attachment` | вкладення контексту |
| `last-prompt`, `atis-latch`, `bridge-session`, `queue-operation`, `custom-title` | службові |

Перевірені поля запису:

```text
uuid, parentUuid, timestamp (ISO-8601), sessionId, cwd, gitBranch,
version, isSidechain (true = робота субагента), userType, entrypoint
```

Для `assistant` додатково: `message.model`, `message.stop_reason`, `message.usage`.
Для `user` з результатом інструмента: `toolUseResult.{stdout, stderr, interrupted, isImage}`.

Це **точно та інформація**, яку вимагає Частина 1 §5: назва інструмента, шлях до файлу,
команда, результат, помилка.

Спосіб читання: `ReadDirectoryChangesW` на каталозі проєкту + інкрементальне дочитування
з останнього зміщення. **Нуль polling, нуль впливу на Claude Code** (Частина 5 §31–32).

### 3.3 Hooks — офіційний механізм

Підтверджено наявністю рядків у бінарнику Claude Code 2.1.260:

| Ідентифікатор | Кількість входжень |
|---|---|
| `PostToolUse` | 80 |
| `SessionStart` | 60 |
| `UserPromptSubmit` | 42 |
| `hook_event_name` | 19 |
| `transcript_path` | 10 |

Hooks налаштовуються у `~/.claude/settings.json`, отримують JSON на stdin
і виконуються самим Claude Code.

**Використання у проєкті — навмисно мінімальне.** Кожен хук — це створення процесу,
а `PreToolUse` блокує виконання інструмента до свого завершення. Це прямо суперечило б
Частині 5 §2 («Bridge не повинен впливати на Claude Code»). Тому:

| Хук | Використовуємо? | Чому |
|---|---|---|
| `SessionStart` | **Так** | запускає Bridge; спрацьовує один раз за сесію |
| `SessionEnd` | Так | коректне завершення; один раз за сесію |
| `Notification` | Так | єдиний спосіб дізнатися «Claude чекає на дозвіл» — саме той сценарій, заради якого будується проєкт |
| `PreToolUse` | **Ні** | блокує Claude Code перед кожним інструментом |
| `PostToolUse` | **Ні** | створює процес після кожного інструмента; ті самі дані вже є в транскрипті |
| `UserPromptSubmit` | Ні | дані є в транскрипті |

---

## 4. Тулчейн — перевірено фактичною збіркою

| Компонент | Статус | Деталі |
|---|---|---|
| MSVC C++ | OK перевірено | 14.44.35207, x64, `Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| CMake | OK | 3.31.6-msvc6 (у складі BuildTools) |
| Ninja | OK | 1.12.1 (у складі BuildTools) |
| Windows SDK | OK | 10.0.26100.0 |
| Node.js | OK | 24.14.0 |
| JDK | OK | 21.0.8 |
| Python | OK | 3.12.10 |
| Git | OK | 2.50.0 |
| Android SDK | встановлено | `Google.AndroidCLI` через winget |

Контрольна збірка: `cl /O2 /EHsc` -> робочий x64-бінарник 140 KB. Тулчейн придатний.

Вільне місце: C: 64 GB, D: 110 GB.

---

## 5. Підсумок — що це означає для архітектури

| Вимога Master Prompt | Рішення | Статус |
|---|---|---|
| Не запускати другий Claude Code (Ч.1 §3) | Bridge лише читає файли й чекає на handle процесу | гарантовано |
| Bridge не стартує з Windows (Ч.5 §3) | запуск через `SessionStart` hook | офіційний механізм |
| Bridge стартує з Claude (Ч.5 §4) | той самий hook | так |
| Bridge завершується з Claude (Ч.5 §4) | `WaitForSingleObject` на root-процесі | 0% CPU |
| Без aggressive polling (Ч.5 §31) | `ReadDirectoryChangesW` + waitable handles | event-driven |
| Без DLL injection / патчів (Ч.5 §5) | не потрібні — усі дані у звичайних файлах | так |
| Без адміністративних прав (Ч.5 §25) | усе в профілі користувача | так |
| Розрізняти Desktop і Code (Ч.5 §10) | різні бінарники, різні джерела | так |
| Кілька сесій (Ч.5 §38) | `sessions/*.json` дає перелік | так |
| Не вигадувати API (Ч.5 §12) | усі три джерела перевірені емпірично | так |

### Чого зробити не можна — чесно

| Бажана інформація | Доступність | Що показуємо |
|---|---|---|
| Точний відсоток прогресу задачі | **недоступно** — Claude Code не публікує такої метрики | не показуємо взагалі |
| Вміст вікна Claude Desktop | доступно лише через screen capture | **заборонено** Частиною 1 §7 |
| Стан до першого запису в транскрипті | затримка до появи першого рядка | `STARTING` |
| Чи Claude «думає» саме зараз | виводиться евристично з транскрипту | `WORKING`, з чесним `UNKNOWN` за браку даних |

Останній рядок — прямий обовʼязок за Частиною 5 §12 і Частиною 6 §42:
**краще `Unknown`, ніж вигадана впевненість.**
