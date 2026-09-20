// transcript.cpp — нормалізація транскрипту Claude Code у події протоколу.

#include "transcript.h"
#include "redact.h"

#include <algorithm>

namespace cm {

namespace {

/// Розбирає позначку часу ISO-8601 виду "2026-09-09T11:39:50.969Z".
/// За невдачі повертає 0 — виклик тоді підставить поточний час.
uint64_t ParseIso8601Ms(std::string_view text) {
    if (text.size() < 19) return 0;

    auto digits = [&](size_t offset, size_t count) -> int {
        int value = 0;
        for (size_t i = 0; i < count; ++i) {
            const char c = text[offset + i];
            if (c < '0' || c > '9') return -1;
            value = value * 10 + (c - '0');
        }
        return value;
    };

    const int year   = digits(0, 4);
    const int month  = digits(5, 2);
    const int day    = digits(8, 2);
    const int hour   = digits(11, 2);
    const int minute = digits(14, 2);
    const int second = digits(17, 2);
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || minute < 0 || second < 0) {
        return 0;
    }

    SYSTEMTIME st{};
    st.wYear   = static_cast<WORD>(year);
    st.wMonth  = static_cast<WORD>(month);
    st.wDay    = static_cast<WORD>(day);
    st.wHour   = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = static_cast<WORD>(second);

    if (text.size() >= 23 && text[19] == '.') {
        const int millis = digits(20, 3);
        if (millis >= 0) st.wMilliseconds = static_cast<WORD>(millis);
    }

    FILETIME ft{};
    if (!::SystemTimeToFileTime(&st, &ft)) return 0;

    ULARGE_INTEGER value{};
    value.LowPart  = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;

    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (value.QuadPart < kEpochDiff100ns) return 0;
    return (value.QuadPart - kEpochDiff100ns) / 10000ULL;
}

/// Готує текст до відправлення: прибирає керівні символи, маскує секрети,
/// обрізає до ліміту. Порядок важливий — маскування має бачити цілий текст,
/// а не вже обрізаний фрагмент.
std::string PrepareText(std::string_view raw) {
    if (raw.empty()) return {};
    std::string clean = SanitizeMultiline(raw);
    if (clean.empty()) return {};
    std::string masked = RedactSecrets(clean);
    return TruncateUtf8(masked, kMaxEventText);
}

/// Те саме, але з межею цілої репліки: далі текст ділиться на частини.
std::string PrepareLongText(std::string_view raw) {
    if (raw.empty()) return {};
    std::string clean = SanitizeMultiline(raw);
    if (clean.empty()) return {};
    std::string masked = RedactSecrets(clean);
    return TruncateUtf8(masked, kMaxOutputBytes);
}

/// Спільний номер для частин однієї репліки. Лічильник, а не позначка часу:
/// дві репліки в одну мілісекунду отримали б однаковий номер і склеїлися б.
uint64_t NextGroupId() {
    static LONG64 counter = 0;
    return static_cast<uint64_t>(::InterlockedIncrement64(&counter));
}

/// Знаходить місце розриву, не довше за `limit` байтів: спершу по межі
/// рядка, потім по пробілу, і лише в останню чергу — по межі символу UTF-8.
/// Розрив посеред слова читається погано, а посеред символу дає кракозябри.
size_t FindSplitPoint(std::string_view text, size_t limit) {
    if (text.size() <= limit) return text.size();

    const std::string_view head = text.substr(0, limit + 1);

    const size_t newline = head.find_last_of('\n');
    if (newline != std::string_view::npos && newline > limit / 2) return newline + 1;

    const size_t space = head.find_last_of(' ');
    if (space != std::string_view::npos && space > limit / 2) return space + 1;

    // Відступаємо назад до початку символу UTF-8.
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return cut > 0 ? cut : limit;
}

Event MakeEvent(const SessionState& session, EventKind kind, uint64_t timestampMs);

/// Ділить готовий текст на частини й віддає їх як окремі події.
///
/// Повний текст репліки не вміщується в кадр, а показувати замість нього
/// перші два кілобайти — означає приховати від користувача більшу частину
/// того, що написав Claude.
template <typename Fill>
void EmitInParts(const SessionState& session, EventKind kind, uint64_t timestampMs,
                 Priority priority, std::string_view content, const EventSink& sink, Fill fill) {
    if (content.empty()) return;

    // Спершу рахуємо частини, щоб кожна знала загальну кількість:
    // застосунок має розуміти, чи дочекався він усього повідомлення.
    std::vector<std::string_view> pieces;
    std::string_view rest(content);
    while (!rest.empty()) {
        const size_t cut = FindSplitPoint(rest, kMaxEventText);
        pieces.push_back(rest.substr(0, cut));
        rest.remove_prefix(cut);
    }

    const uint64_t groupId = pieces.size() > 1 ? NextGroupId() : 0;
    for (size_t i = 0; i < pieces.size(); ++i) {
        Event event = MakeEvent(session, kind, timestampMs);
        event.priority = priority;
        if (pieces.size() > 1) {
            event.groupId = groupId;
            event.part = static_cast<uint16_t>(i);
            event.partCount = static_cast<uint16_t>(pieces.size());
        }
        fill(event, pieces[i]);
        sink(std::move(event));
    }
}

/// Текст події частинами: репліка, запит чи вивід команди.
void EmitTextEvents(const SessionState& session, EventKind kind, uint64_t timestampMs,
                    Priority priority, const std::string& text, const EventSink& sink,
                    bool isError = false, size_t imageCount = 0) {
    EmitInParts(session, kind, timestampMs, priority, text, sink,
                [isError, imageCount](Event& event, std::string_view piece) {
                    event.text = std::string(piece);
                    event.isError = isError;
                    // Зображення рахуються один раз — у першій частині.
                    if (event.part == 0) event.imageCount = static_cast<uint16_t>(imageCount);
                });
}

/// Дія частинами: довга команда йде повністю, а не обрізаною.
void EmitActivityEvents(const SessionState& session, ActivityAction action,
                        const std::string& target, uint64_t timestampMs, const EventSink& sink) {
    EmitInParts(session, EventKind::Activity, timestampMs, Priority::Medium, target, sink,
                [action](Event& event, std::string_view piece) {
                    event.action = action;
                    event.target = std::string(piece);
                });
}

/// Останній сегмент шляху — те, що показується в UI як назва проєкту.
std::string LastPathSegment(std::string_view path) {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.remove_suffix(1);
    }
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

Event MakeEvent(const SessionState& session, EventKind kind, uint64_t timestampMs) {
    Event event;
    event.sessionId = session.sessionId;
    event.kind = kind;
    event.timestampMs = timestampMs != 0 ? timestampMs : NowUnixMs();
    return event;
}

/// Стани, у яких відповідь Claude ще триває.
bool IsTurnState(ClaudeState state) {
    return state == ClaudeState::Working || state == ClaudeState::Error ||
           state == ClaudeState::Waiting;
}

/// Переводить сесію в новий стан і повідомляє про це окремою подією.
///
/// Подія йде лише за справжньої зміни — інакше кожен рядок транскрипту
/// дублював би той самий стан. І навпаки: тиха зміна без події означала б,
/// що телефон дізнається про неї лише з наступного знімка, а знімок під час
/// активної роботи не надсилається зовсім. Саме так «очікує» трималося
/// на екрані протягом десятка реплік Claude.
///
/// Разом зі станом ведеться облік відповіді — від вашого запиту до кінця
/// відповіді Claude. Помилка інструмента чи очікування дозволу відповідь
/// не переривають: після них Claude продовжує ту саму відповідь.
void TransitionState(SessionState& session, ClaudeState next, uint64_t timestampMs,
                     const EventSink& sink, std::string_view reason = {}) {
    if (session.state == next) {
        // Той самий стан подій не потребує, крім одного випадку: стан уже
        // «очікує», а відповідь іще відкрита. Коли вона завершується,
        // повідомляємо, скільки тривала, — інакше тривалість загубилася б.
        const bool closesTurn = next == ClaudeState::Idle || next == ClaudeState::Limited;
        const bool closesKnownTurn = closesTurn &&
                                     session.turnStartKnown && session.turnStartedAtMs != 0;
        if (!closesKnownTurn) {
            if (closesTurn) {
                session.turnStartedAtMs = 0;
                session.turnStartKnown = false;
            }
            return;
        }
    }
    session.state = next;
    // Будь-який інший стан означає, що ліміт уже не діє.
    if (next != ClaudeState::Limited) session.limitResetAtMs = 0;

    const uint64_t at = timestampMs != 0 ? timestampMs : NowUnixMs();

    Event event = MakeEvent(session, EventKind::Status, timestampMs);
    event.state = next;
    if (!reason.empty()) event.text = std::string(reason);
    event.priority = Priority::High;

    if (next == ClaudeState::Idle || next == ClaudeState::Limited) {
        if (session.turnStartKnown && session.turnStartedAtMs != 0 &&
            at >= session.turnStartedAtMs) {
            session.lastTurnMs = at - session.turnStartedAtMs;
            event.turnMs = session.lastTurnMs;
        }
        session.turnStartedAtMs = 0;
        session.turnStartKnown = false;
    } else if (IsTurnState(next)) {
        if (session.turnStartedAtMs == 0) session.turnStartedAtMs = at;
        if (session.turnStartKnown) event.turnStartedAtMs = session.turnStartedAtMs;
    }

    sink(std::move(event));
}

/// Збирає текст запиту з поля content: це буває рядок або масив блоків,
/// де поруч із текстом трапляються зображення.
void CollectPromptText(const json::Value& content, std::string& text, size_t& imageCount) {
    if (content.isString()) {
        text = content.asString();
        return;
    }
    if (!content.isArray()) return;
    for (const json::Value& item : content.asArray()) {
        const std::string itemType = item["type"].asStringOr("");
        if (itemType == "text") {
            if (!text.empty()) text += '\n';
            text += item["text"].asStringOr("");
        } else if (itemType == "image") {
            ++imageCount;
        }
    }
}

/// «/compact», «/clear» — команди Claude Code, а не повідомлення для Claude.
bool LooksLikeSlashCommand(std::string_view text) {
    if (text.size() < 2 || text[0] != '/') return false;
    size_t i = 1;
    while (i < text.size()) {
        const char c = text[i];
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
        if (!word) break;
        ++i;
    }
    return i > 1 && (i == text.size() || text[i] == ' ' || text[i] == '\n');
}

/// Службовий запис, оформлений як запит користувача.
///
/// Claude Code кладе у чергу власні повідомлення в кутових дужках:
/// `<task-notification>` про завершення фонової команди, `<command-message>`
/// та подібні. У стрічці вони виглядали як «ваше повідомлення», хоча
/// користувач їх не писав.
bool LooksLikeServiceWrapper(std::string_view text) {
    const size_t first = text.find_first_not_of(" \n\r\t");
    return first != std::string_view::npos && text[first] == '<';
}

size_t PromptHash(std::string_view preparedText) {
    return std::hash<std::string_view>{}(preparedText);
}

/// Показує повідомлення користувача в стрічці — рівно один раз.
///
/// Повідомлення, надіслане, поки Claude працює, трапляється в транскрипті
/// двічі: коли його поставлено в чергу і коли Claude його отримав — посеред
/// відповіді або як початок наступної. Для вас це одне повідомлення: перша
/// поява його показує, друга лише гасить очікуваний повтор. Однаковий текст
/// у різних відповідях («продовжуй») показується щоразу — кожен показ чекає
/// рівно на один повтор.
void EmitPromptOnce(SessionState& session, std::string text, size_t imageCount,
                    uint64_t timestampMs, const EventSink& sink, bool expectDuplicate) {
    // Самих зображень на телефон не передаємо — лише їхню кількість: інакше
    // запит «подивись на фото» незрозумілий, а виносити самі знімки з ноутбука
    // не можна (Частина 4 §17 Master Prompt).
    const std::string prepared = PrepareLongText(text);
    if (prepared.empty() && imageCount == 0) return;

    // Однаковий текст із різною кількістю зображень — різні повідомлення.
    const size_t hash = PromptHash(prepared) ^
                        static_cast<size_t>(imageCount * 0x9E3779B97F4A7C15ULL);
    auto& expected = session.expectedPromptDuplicates;
    const auto seen = std::find(expected.begin(), expected.end(), hash);
    if (seen != expected.end()) {
        expected.erase(seen);
        return;
    }

    if (prepared.empty()) {
        // Повідомлення без тексту — самі лише зображення.
        Event event = MakeEvent(session, EventKind::Prompt, timestampMs);
        event.priority = Priority::Medium;
        event.imageCount = static_cast<uint16_t>(imageCount);
        sink(std::move(event));
    } else {
        EmitTextEvents(session, EventKind::Prompt, timestampMs, Priority::Medium, prepared, sink,
                       /*isError=*/false, imageCount);
    }

    if (expectDuplicate) {
        expected.push_back(hash);
        // Межа — щоб очікування не накопичувались, якщо повтор так і не прийде.
        while (expected.size() > 16) expected.pop_front();
    }
}

/// Повідомлення прибрали з черги без доставки — його повтору вже не буде.
void ForgetExpectedPrompt(SessionState& session, std::string_view text) {
    if (text.empty()) return;
    const size_t hash = PromptHash(PrepareLongText(text));
    auto& expected = session.expectedPromptDuplicates;
    const auto seen = std::find(expected.begin(), expected.end(), hash);
    if (seen != expected.end()) expected.erase(seen);
}

/// Скільки тиші означає, що відповідь обірвалась, а не триває.
/// З великим запасом: Claude буває думає й по десять хвилин.
constexpr uint64_t kAbandonedTurnMs = 30 * 60 * 1000;

/// Час скидання ліміту з тексту синтетичної репліки: «… · resets 10:30pm
/// (Europe/Kyiv)» → «10:30pm (Europe/Kyiv)». Перетворення у звичний вигляд —
/// справа застосунку, він знає мову й формат часу.
std::string ExtractLimitReset(const json::Value& content) {
    if (!content.isArray()) return {};
    for (const json::Value& item : content.asArray()) {
        if (item["type"].asStringOr("") != "text") continue;
        const std::string text = item["text"].asStringOr("");
        const size_t at = text.find("resets ");
        if (at != std::string::npos) return SanitizeText(text.substr(at + 7));
    }
    return {};
}

}  // namespace

// ── Відображення інструментів ────────────────────────────────────────────────

ActivityAction MapToolToAction(std::string_view toolName) {
    struct Mapping { std::string_view name; ActivityAction action; };

    // Перелік охоплює інструменти, які реально трапляються в транскриптах.
    // Усе інше свідомо потрапляє в Other.
    static constexpr Mapping kMap[] = {
        {"Read",         ActivityAction::ReadFile},
        {"NotebookRead", ActivityAction::ReadFile},
        {"Edit",         ActivityAction::EditFile},
        {"MultiEdit",    ActivityAction::EditFile},
        {"NotebookEdit", ActivityAction::EditFile},
        {"Write",        ActivityAction::WriteFile},
        {"Bash",         ActivityAction::RunCommand},
        {"PowerShell",   ActivityAction::RunCommand},
        {"BashOutput",   ActivityAction::RunCommand},
        {"Glob",         ActivityAction::Search},
        {"Grep",         ActivityAction::Search},
        {"WebFetch",     ActivityAction::WebFetch},
        {"WebSearch",    ActivityAction::WebFetch},
        {"Task",         ActivityAction::Task},
        {"Agent",        ActivityAction::Subagent},
        {"TodoWrite",    ActivityAction::Task},
    };

    for (const Mapping& mapping : kMap) {
        if (toolName == mapping.name) return mapping.action;
    }
    return ActivityAction::Other;
}

std::string ExtractActionTarget(std::string_view toolName, const json::Value& input) {
    if (!input.isObject()) return {};

    // З аргументів беремо рівно одне поле — те, що описує ціль дії.
    // Решта аргументів (вміст файлу, текст заміни, промпт субагента)
    // на телефон не потрапляє ніколи (Частина 3 §17 Master Prompt).
    static constexpr std::string_view kTargetFields[] = {
        "file_path", "path", "notebook_path", "command", "pattern", "url", "query",
    };

    for (std::string_view field : kTargetFields) {
        const json::Value& value = input[field];
        if (!value.isString() || value.asString().empty()) continue;

        std::string target = value.asString();

        // Шлях до файлу з секретами не показуємо навіть як назву:
        // сама наявність файлу може бути чутливою.
        if ((field == "file_path" || field == "path" || field == "notebook_path") &&
            IsSensitivePath(target)) {
            return "«файл із секретами»";
        }

        if (field == "command") {
            // Команду показуємо як вона є: багаторядковий сценарій,
            // склеєний в один рядок, прочитати неможливо, а саме за командою
            // й видно, що зараз відбувається.
            target = RedactSecrets(SanitizeMultiline(target));
        } else if (field == "query" || field == "url") {
            target = RedactSecrets(SanitizeText(target));
        } else {
            target = SanitizeText(target);
        }

        // Ціль іде повністю — довга команда ділиться на частини так само,
        // як текст. Скільки з неї показати, вирішує застосунок: у картці
        // задачі лише початок, у стрічці — уся команда.
        return TruncateUtf8(target, kMaxTargetBytes);
    }

    // Інструмент без розпізнаної цілі — показуємо саму назву інструмента,
    // це чесніше, ніж порожнє поле.
    (void)toolName;
    return {};
}

// ── Обробка рядка ────────────────────────────────────────────────────────────

void FlushHeldTurnEnd(SessionState& session, const EventSink& sink) {
    if (session.heldTurnEnd.empty()) return;

    std::vector<Event> held = std::move(session.heldTurnEnd);
    session.heldTurnEnd.clear();
    session.heldTurnEndMessageId.clear();
    for (Event& event : held) sink(std::move(event));
}

bool ProcessTranscriptLine(std::string_view line, SessionState& session, const EventSink& sink) {
    if (line.empty()) return false;

    // Захист від аномально довгого рядка: обробляти його немає сенсу,
    // а пам'ять він з'їсть (Частина 5 §33 Master Prompt).
    if (line.size() > kMaxLineBytes) return false;

    json::Value root;
    if (!json::Parse(line, root) || !root.isObject()) {
        // Пошкоджений рядок — не привід зупиняти читання. Claude Code міг
        // саме зараз дописувати цей рядок; наступне читання його добере.
        return false;
    }

    const std::string type = root["type"].asStringOr("");
    if (type.empty()) return false;

    const uint64_t timestampMs = ParseIso8601Ms(root["timestamp"].asStringOr(""));

    // Притриманий кінець відповіді оголошується, щойно з'являється будь-що,
    // крім наступного блоку тієї самої репліки: отже, її дописано.
    if (!session.heldTurnEnd.empty()) {
        const bool sameReply = type == "assistant" &&
                               !session.heldTurnEndMessageId.empty() &&
                               root["message"]["id"].asStringOr("") == session.heldTurnEndMessageId;
        if (!sameReply) FlushHeldTurnEnd(session, sink);
    }

    // Метадані сесії оновлюються з будь-якого запису, де вони є.
    if (session.cwd.empty()) {
        session.cwd = root["cwd"].asStringOr("");
    }
    // Назва проєкту обчислюється незалежно від того, звідки прийшов cwd:
    // він міг потрапити сюди з реєстру сесій ще до читання транскрипту.
    if (session.project.empty() && !session.cwd.empty()) {
        session.project = LastPathSegment(session.cwd);
        if (session.titleSource == TitleSource::None && !session.project.empty()) {
            session.title = session.project;
            session.titleSource = TitleSource::Path;
        }
    }
    if (session.gitBranch.empty()) {
        const std::string branch = root["gitBranch"].asStringOr("");
        // "HEAD" означає від'єднаний стан репозиторію. Показувати це
        // користувачу немає сенсу — краще не показувати гілку взагалі.
        if (branch != "HEAD") session.gitBranch = branch;
    }

    // ── Назва задачі, задана явно ────────────────────────────────────────
    if (type == "custom-title") {
        const std::string title = root["customTitle"].asStringOr("");
        if (!title.empty()) {
            session.title = TruncateUtf8(SanitizeText(title), 120);
            session.titleSource = TitleSource::Custom;
        }
        return true;
    }

    // ── Назва з запиту користувача ───────────────────────────────────────
    if (type == "last-prompt") {
        // Явно задана назва має вищий пріоритет і не перезаписується.
        if (session.titleSource == TitleSource::Custom) return true;

        // Береться ПЕРШИЙ запит сесії, а не останній. Поле lastPrompt
        // оновлюється на кожну репліку користувача, тому назва задачі
        // інакше стрибала б на випадкові фрази на кшталт «продовжуй»
        // замість того, щоб описувати суть роботи.
        if (session.titleSource == TitleSource::Prompt) return true;

        const std::string prompt = root["lastPrompt"].asStringOr("");
        if (!prompt.empty()) {
            session.title = TruncateUtf8(RedactSecrets(SanitizeText(prompt)), 80);
            session.titleSource = TitleSource::Prompt;
        }
        return true;
    }

    // ── Відповідь асистента ──────────────────────────────────────────────
    if (type == "assistant") {
        const json::Value& message = root["message"];
        const json::Value& content = message["content"];

        // Розмір контексту з облікових даних відповіді.
        //
        // Claude Code показує саме цю величину як «context window»: майже
        // весь обсяг припадає на прочитане з кешу, а вхід і створення кешу
        // додають решту.
        const json::Value& usage = message["usage"];
        if (usage.isObject()) {
            const int64_t cacheRead = usage["cache_read_input_tokens"].asInt(0);
            const int64_t cacheWrite = usage["cache_creation_input_tokens"].asInt(0);
            const int64_t input = usage["input_tokens"].asInt(0);

            const int64_t total = cacheRead + cacheWrite + input;
            if (total > 0) session.contextTokens = static_cast<uint64_t>(total);
        }

        if (session.model.empty()) {
            session.model = message["model"].asStringOr("");
        }

        // Будь-яка репліка асистента посеред ходу — доказ, що Claude працює:
        // думає, пише відповідь чи готує виклик інструмента. Раніше робота
        // визнавалась лише за викликом інструмента, і поки Claude просто
        // думав і писав текст, задача лишалася «очікує».
        const std::string stopReason = message["stop_reason"].asStringOr("");
        const bool turnEnded = stopReason == "end_turn" || stopReason == "stop_sequence";
        if (!turnEnded && content.isArray() && !content.asArray().empty()) {
            session.idleDeferred = false;
            TransitionState(session, ClaudeState::Working, timestampMs, sink);
        }

        if (content.isArray()) {
            for (const json::Value& item : content.asArray()) {
                const std::string itemType = item["type"].asStringOr("");

                if (itemType == "tool_use") {
                    const std::string toolName = item["name"].asStringOr("");
                    if (toolName.empty()) continue;

                    // Подія activity стану не несе, тож про роботу повідомляємо
                    // окремо — інакше мітка задачі лишалася б старою.
                    TransitionState(session, ClaudeState::Working, timestampMs, sink);

                    const ActivityAction action = MapToolToAction(toolName);
                    std::string target = ExtractActionTarget(toolName, item["input"]);
                    if (target.empty()) target = SanitizeText(toolName);

                    session.currentAction = action;
                    // Картка задачі показує лише початок — повна ціль іде в стрічку.
                    session.currentTarget = TruncateUtf8(target, kMaxActivityPreview);
                    session.hasActivity = true;

                    // Незавершений виклик інструмента — саме той доказ,
                    // на якому тримається стан Working.
                    session.pendingToolUse = true;
                    session.pendingSinceMs = timestampMs != 0 ? timestampMs : NowUnixMs();

                    EmitActivityEvents(session, action, target, timestampMs, sink);

                } else if (itemType == "text" || itemType == "thinking") {
                    // Репліка йде повністю, частинами. Пріоритет середній, а не
                    // низький: відповідь — те, заради чого застосунок відкривають,
                    // і губити її при заповненні черги не можна.
                    //
                    // Блок thinking із текстом — теж репліка: Claude Desktop показує
                    // його в чаті, і частина повідомлень Claude записується саме так,
                    // а не блоком text. Раніше такі блоки пропускались, і посеред
                    // довгої відповіді текст Claude просто переставав доходити до
                    // телефона. Порожній thinking (лише підпис) тексту не несе.
                    const std::string raw = itemType == "text" ? item["text"].asStringOr("")
                                                               : item["thinking"].asStringOr("");
                    const size_t last = raw.find_last_not_of(" \n\r\t");
                    if (last == std::string::npos) continue;
                    EmitTextEvents(session, EventKind::Output, timestampMs, Priority::Medium,
                                   PrepareLongText(std::string_view(raw).substr(0, last + 1)), sink);
                }
            }
        }

        // Кінець ходу: Claude відповів і чекає на наступний запит.
        if (turnEnded) {
            session.pendingToolUse = false;
            session.hasActivity = false;

            // Якщо посеред відповіді ви надіслали ще повідомлення, Claude Code
            // одразу візьметься за нього. Для вас це та сама розмова: стан
            // і таймер не перериваються на мить між двома відповідями.
            // Вичерпаний ліміт — не звичайний кінець: Claude Code записує його
            // синтетичною реплікою з error = "rate_limit", і продовжити можна
            // лише після скидання. Чекати на повідомлення з черги тут марно.
            const bool limitHit = root["error"].asStringOr("") == "rate_limit";
            const bool queuedDuringTurn = !limitHit && session.queuedPrompts > 0 &&
                                          session.turnStartKnown &&
                                          session.lastEnqueueMs >= session.turnStartedAtMs;

            // Подію кінця відповіді притримуємо, доки не прийдуть решта блоків
            // цієї репліки: Claude Code пише їх окремими рядками, і ознака кінця
            // ходу є вже в першому. Інакше «ГОТОВО» стояло б перед текстом.
            // Стан при цьому змінюється одразу — затримується лише позначка.
            const std::string messageId = message["id"].asStringOr("");
            const EventSink hold = [&session](Event&& event) {
                session.heldTurnEnd.push_back(std::move(event));
            };
            const EventSink& endSink = messageId.empty() ? sink : hold;
            const bool continuesHeld = !messageId.empty() && !session.heldTurnEnd.empty() &&
                                       messageId == session.heldTurnEndMessageId;

            if (continuesHeld) {
                // Наступний блок тієї самої репліки: кінець відповіді — тепер
                // на ньому, і тривалість відповіді зростає відповідно.
                Event& held = session.heldTurnEnd.back();
                if (timestampMs > held.timestampMs) {
                    if (held.turnMs > 0) {
                        held.turnMs += timestampMs - held.timestampMs;
                        session.lastTurnMs = held.turnMs;
                    }
                    held.timestampMs = timestampMs;
                }
            } else if (limitHit) {
                session.idleDeferred = false;
                const std::string reset = ExtractLimitReset(content);
                TransitionState(session, ClaudeState::Limited, timestampMs, endSink, reset);
                // Момент скидання запам'ятовуємо: коли він настане, Bridge сам
                // поверне задачу в очікування (SessionManager::ReleaseExpiredLimits).
                session.limitResetAtMs =
                    ParseLimitResetMs(reset, timestampMs != 0 ? timestampMs : NowUnixMs());
            } else if (queuedDuringTurn) {
                session.idleDeferred = true;
            } else {
                TransitionState(session, ClaudeState::Idle, timestampMs, endSink);
            }
            if (!messageId.empty() && !session.heldTurnEnd.empty()) {
                session.heldTurnEndMessageId = messageId;
            }
        }

        session.lastUpdateMs = timestampMs != 0 ? timestampMs : NowUnixMs();
        return true;
    }

    // ── Результат інструмента ────────────────────────────────────────────
    if (type == "user") {
        const json::Value& content = root["message"]["content"];
        bool sawToolResult = false;
        bool isError = false;

        if (content.isArray()) {
            for (const json::Value& item : content.asArray()) {
                if (item["type"].asStringOr("") != "tool_result") continue;
                sawToolResult = true;
                if (item["is_error"].asBool(false)) isError = true;
            }
        }

        if (!sawToolResult) {
            // Запит користувача — друга половина розмови. Без нього стрічка
            // на телефоні показувала б відповіді, не показуючи, на що саме.
            std::string promptText;
            size_t imageCount = 0;
            CollectPromptText(content, promptText, imageCount);

            const bool sidechain = root["isSidechain"].asBool(false);

            // Перервати відповідь може лише користувач, і Claude Code записує
            // це окремим рядком. Це кінець ходу, а не новий запит: задача
            // переходить в очікування, а службовий текст не потрапляє
            // в стрічку як «ваше повідомлення».
            if (!sidechain && StartsWith(promptText, "[Request interrupted by user")) {
                session.pendingToolUse = false;
                session.hasActivity = false;
                TransitionState(session, ClaudeState::Idle, timestampMs, sink, "перервано");
                session.lastUpdateMs = timestampMs != 0 ? timestampMs : NowUnixMs();
                return true;
            }

            // Службові записи запитами не є: метадані (isMeta), стислий
            // переказ розмови після ущільнення (isCompactSummary), завдання
            // субагентам (isSidechain) та обгортки команд Claude Code
            // у кутових дужках.
            const bool service = sidechain ||
                                 root["isMeta"].asBool(false) ||
                                 root["isCompactSummary"].asBool(false) ||
                                 root["isVisibleInTranscriptOnly"].asBool(false);

            const size_t first = promptText.find_first_not_of(" \n\r\t");
            const bool wrapper = LooksLikeServiceWrapper(promptText);
            const bool hasContent = first != std::string::npos || imageCount > 0;

            if (!service && !wrapper && hasContent) {
                // Те саме повідомлення могло з'явитися в стрічці ще тоді, коли
                // його поставили в чергу, — вдруге його не показуємо.
                EmitPromptOnce(session, promptText, imageCount, timestampMs, sink,
                               /*expectDuplicate=*/false);
                const bool continuesQueued = session.idleDeferred;
                session.idleDeferred = false;

                // Новий запит після завершеної відповіді — новий відлік часу.
                // Запит, надісланий, поки Claude ще відповідає, відлік не скидає.
                //
                // Виняток — «відповідь», у якій понад пів години не було жодного
                // запису: насправді вона обірвалась (Claude Code закрили чи він
                // упав, не записавши кінця). Новий запит після такої тиші — нова
                // відповідь, інакше таймер рахував би від давно забутого запиту.
                const uint64_t promptAt = timestampMs != 0 ? timestampMs : NowUnixMs();
                const bool abandoned = IsTurnState(session.state) && !continuesQueued &&
                                       session.lastUpdateMs != 0 &&
                                       promptAt > session.lastUpdateMs &&
                                       promptAt - session.lastUpdateMs > kAbandonedTurnMs;
                if (!IsTurnState(session.state) || abandoned) {
                    session.turnStartedAtMs = promptAt;
                    session.turnStartKnown = true;
                }

                // Щойно запит надіслано, Claude Code береться до роботи.
                // Чекати на перший виклик інструмента не можна: до нього
                // Claude може довго думати й писати текст.
                TransitionState(session, ClaudeState::Working, timestampMs, sink);
            }

            session.lastUpdateMs = timestampMs != 0 ? timestampMs : NowUnixMs();
            return true;
        }

        const json::Value& result = root["toolUseResult"];
        std::string summary;

        if (result.isObject()) {
            const std::string stderrText = result["stderr"].asStringOr("");
            const std::string stdoutText = result["stdout"].asStringOr("");

            // Ознакою помилки є ПРАПОРЕЦЬ is_error, а не сам факт запису
            // в stderr.
            //
            // Багато програм пишуть туди звичайні повідомлення: git — стан
            // операції, cmake — хід збірки, а сама оболонка — службове
            // «Shell cwd was reset». Позначати це помилкою означало б
            // засипати користувача червоними рядками там, де все гаразд.
            // Обидва потоки — повністю. Раніше довгий stderr витісняв stdout
            // зовсім, і частина виводу просто не доходила до телефона.
            summary = stdoutText;
            if (!stderrText.empty()) {
                if (!summary.empty()) summary += '\n';
                summary += stderrText;
            }

            if (result["interrupted"].asBool(false)) {
                isError = true;
                if (summary.empty()) summary = "перервано";
            }
        } else if (result.isString()) {
            summary = result.asString();
        }

        session.pendingToolUse = false;
        session.lastUpdateMs = timestampMs != 0 ? timestampMs : NowUnixMs();

        // Подія result сама по собі мітки задачі не змінює, тож про зміну
        // стану повідомляємо окремо.
        TransitionState(session, isError ? ClaudeState::Error : ClaudeState::Working,
                        timestampMs, sink);

        // Успішний результат без тексту не несе інформації: користувач уже
        // бачить саму дію в стрічці, а «порожній успіх» лише засмічував би
        // UI і марнував трафік (Частина 2 §6 Master Prompt).
        if (summary.empty() && !isError) return true;

        std::string prepared = PrepareLongText(summary);
        if (prepared.empty()) {
            if (!isError) return true;
            prepared = "помилка без опису";
        }

        // Вивід — повністю, частинами. Помилки не мають губитися
        // при переповненні черги.
        EmitTextEvents(session, EventKind::Result, timestampMs,
                       isError ? Priority::High : Priority::Medium, prepared, sink, isError);

        return true;
    }

    // ── Черга повідомлень ────────────────────────────────────────────────
    //
    // Повідомлення, надіслане посеред роботи, Claude Code спершу ставить
    // у чергу. Показуємо його одразу: ви маєте бачити своє повідомлення
    // в стрічці, не чекаючи, поки Claude до нього дійде.
    if (type == "queue-operation") {
        const std::string operation = root["operation"].asStringOr("");
        const std::string queued = root["content"].asStringOr("");

        if (operation == "enqueue") {
            if (session.queuedPrompts < 64) ++session.queuedPrompts;
            session.lastEnqueueMs = timestampMs != 0 ? timestampMs : NowUnixMs();
            // Службові записи Claude Code (`<task-notification>` про завершення
            // фонової команди й подібні) теж проходять через чергу, але вашими
            // повідомленнями не є — у стрічці їм не місце.
            if (!queued.empty() && !LooksLikeSlashCommand(queued) &&
                !LooksLikeServiceWrapper(queued)) {
                EmitPromptOnce(session, queued, 0, timestampMs, sink, /*expectDuplicate=*/true);
            }
        } else if (operation == "dequeue" || operation == "remove") {
            if (session.queuedPrompts > 0) --session.queuedPrompts;

            if (operation == "remove") {
                // Прибране з черги без доставки повідомлення дубліката вже не матиме.
                ForgetExpectedPrompt(session, queued);

                // Якщо кінець відповіді відкладали заради нього, а нової
                // відповіді не буде, — завершуємо відкладене.
                if (session.idleDeferred && session.queuedPrompts == 0) {
                    session.idleDeferred = false;
                    TransitionState(session, ClaudeState::Idle, timestampMs, sink);
                }
            }
        }
        return true;
    }

    // Повідомлення з черги, яке Claude отримав посеред відповіді.
    if (type == "attachment") {
        const json::Value& attachment = root["attachment"];
        if (attachment["type"].asStringOr("") != "queued_command") return true;
        if (attachment["commandMode"].asStringOr("prompt") != "prompt") return true;
        if (attachment["origin"]["kind"].asStringOr("human") != "human") return true;

        std::string text;
        size_t imageCount = 0;
        CollectPromptText(attachment["prompt"], text, imageCount);

        if (LooksLikeServiceWrapper(text)) return true;

        EmitPromptOnce(session, text, imageCount, timestampMs, sink, /*expectDuplicate=*/false);
        return true;
    }

    // Решта типів (atis-latch, bridge-session тощо) не несуть інформації
    // для UI. Розпізнано — але подій не породжує.
    return true;
}

// ── Початок відповіді, що триває ─────────────────────────────────────────────

namespace {

enum class TurnMark { None, Prompt, End };

/// Що означає рядок для пошуку початку відповіді: запит, кінець відповіді
/// чи ні те, ні інше. Рішення ухвалює той самий розбір, що й під час
/// звичайного читання, — на чистій копії стану, тож поточна задача не змінюється.
TurnMark ClassifyTurnLine(std::string_view line, uint64_t& promptAt) {
    const EventSink discard = [](Event&&) {};

    // Дешевий відбір до повного розбору JSON. Лапки полів самого запису
    // не екрановані, а всередині рядкових значень — екрановані, тож збіг
    // означає справжнє поле, а не текст повідомлення.
    if (line.find("\"type\":\"assistant\"") != std::string_view::npos) {
        if (line.find("\"stop_reason\":\"end_turn\"") == std::string_view::npos &&
            line.find("\"stop_reason\":\"stop_sequence\"") == std::string_view::npos) {
            return TurnMark::None;
        }
        SessionState scratch;
        scratch.state = ClaudeState::Working;
        scratch.turnStartKnown = true;
        scratch.turnStartedAtMs = 1;
        ProcessTranscriptLine(line, scratch, discard);
        const bool ended = scratch.state == ClaudeState::Idle ||
                           scratch.state == ClaudeState::Limited;
        return ended ? TurnMark::End : TurnMark::None;
    }

    if (line.find("\"type\":\"user\"") != std::string_view::npos) {
        if (line.find("\"type\":\"tool_result\"") != std::string_view::npos) return TurnMark::None;

        SessionState scratch;
        scratch.state = ClaudeState::Idle;
        ProcessTranscriptLine(line, scratch, discard);
        if (scratch.turnStartKnown && scratch.state == ClaudeState::Working) {
            promptAt = scratch.turnStartedAtMs;
            return TurnMark::Prompt;
        }
        // Переривання — теж кінець відповіді.
        if (line.find("[Request interrupted by user") != std::string_view::npos) return TurnMark::End;
    }
    return TurnMark::None;
}

/// Шукає назад від `end` запит, з якого почалась відповідь, що триває.
///
/// Файл читається порціями від кінця до початку, аж до кінця попередньої
/// відповіді. Запитом-початком вважається найраніший запит після нього:
/// повідомлення, надіслані посеред роботи, відлік не скидають.
///
/// @returns мітку часу запиту; 0 — початку не знайдено в межах пошуку
uint64_t FindTurnStartBefore(HANDLE file, uint64_t end) {
    // Межа пошуку: навіть дуже довга відповідь із великими файлами
    // вкладається в десятки мегабайтів, а далі шукати немає сенсу.
    constexpr uint64_t kMaxScanBytes = 64ULL * 1024 * 1024;
    constexpr size_t kChunkBytes = 1024 * 1024;

    const uint64_t limit = end > kMaxScanBytes ? end - kMaxScanBytes : 0;
    uint64_t pos = end;
    uint64_t earliestPrompt = 0;

    std::string carry;  // початок рядка, що тягнеться з іще не прочитаної частини
    std::string data;

    while (pos > limit) {
        const size_t size = static_cast<size_t>(std::min<uint64_t>(kChunkBytes, pos - limit));
        const uint64_t from = pos - size;

        LARGE_INTEGER offset{};
        offset.QuadPart = static_cast<LONGLONG>(from);
        if (!::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) return 0;

        data.assign(size, '\0');
        DWORD read = 0;
        if (!::ReadFile(file, data.data(), static_cast<DWORD>(size), &read, nullptr) || read != size) {
            return 0;
        }
        data += carry;

        // Рядки — від кінця порції до початку. Відрізок перед першим
        // переводом рядка може бути неповним: він починається раніше.
        size_t lineEnd = data.size();
        while (lineEnd > 0) {
            const size_t newline = data.rfind('\n', lineEnd - 1);
            if (newline == std::string::npos) break;

            std::string_view line(data.data() + newline + 1, lineEnd - newline - 1);
            lineEnd = newline;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line.size() > kMaxLineBytes) continue;

            uint64_t promptAt = 0;
            const TurnMark mark = ClassifyTurnLine(line, promptAt);
            if (mark == TurnMark::End) return earliestPrompt;
            if (mark == TurnMark::Prompt) earliestPrompt = promptAt;
        }

        carry.assign(data.data(), lineEnd);
        // Аномально довгий рядок не накопичуємо: пам'ять має лишатися обмеженою.
        if (carry.size() > kMaxLineBytes) carry.clear();
        pos = from;
    }

    // Межу пошуку досягнуто раніше за початок файлу — справжній початок
    // відповіді може лежати ще далі, і вгадувати його не можна.
    if (limit > 0) return 0;

    // Дійшли до початку файлу: лишок — перший рядок, і він повний.
    if (!carry.empty()) {
        uint64_t promptAt = 0;
        const TurnMark mark = ClassifyTurnLine(carry, promptAt);
        if (mark == TurnMark::Prompt) earliestPrompt = promptAt;
    }
    return earliestPrompt;
}

}  // namespace

// ── Читання файлу ────────────────────────────────────────────────────────────

void TranscriptReader::SplitLines(std::string_view buffer,
                                  std::vector<std::string_view>& lines,
                                  size_t& consumedBytes) {
    lines.clear();
    consumedBytes = 0;

    size_t start = 0;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] != '\n') continue;

        std::string_view line = buffer.substr(start, i - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) lines.push_back(line);

        start = i + 1;
        consumedBytes = start;
    }
    // Хвіст без переводу рядка лишається необробленим: файл дописується
    // просто зараз, і рядок може бути неповним.
}

size_t TranscriptReader::ReadNew(SessionState& session, const EventSink& sink) {
    if (session.transcriptPath.empty()) return 0;

    Handle file(::CreateFileW(session.transcriptPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return 0;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) return 0;
    const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);

    // Файл став меншим — його підмінили або сесію перезапустили.
    // Читати зі старого зміщення означало б розбирати сміття.
    if (fileSize < session.transcriptOffset) {
        session.transcriptOffset = 0;
    }
    if (fileSize == session.transcriptOffset) {
        // Нових рядків немає — репліку дописано, і притриманий кінець
        // відповіді можна оголосити.
        FlushHeldTurnEnd(session, sink);
        return 0;
    }

    // За раз читаємо обмежену порцію. Якщо накопичилося більше,
    // решта дочитається наступним викликом — це тримає пікову пам'ять
    // під контролем незалежно від розміру файлу.
    const uint64_t available = fileSize - session.transcriptOffset;
    const size_t toRead = static_cast<size_t>(std::min<uint64_t>(available, kMaxReadChunk));

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(session.transcriptOffset);
    if (!::SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) return 0;

    std::string buffer(toRead, '\0');
    DWORD read = 0;
    if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(toRead), &read, nullptr)) {
        return 0;
    }
    buffer.resize(read);
    if (buffer.empty()) return 0;

    std::vector<std::string_view> lines;
    size_t consumed = 0;
    SplitLines(buffer, lines, consumed);

    // Якщо жодного повного рядка немає, але прочитано вже дуже багато —
    // рядок аномально довгий. Пропускаємо його, інакше читач застрягне назавжди.
    if (consumed == 0 && read >= kMaxReadChunk) {
        session.transcriptOffset += read;
        return 0;
    }

    size_t processed = 0;
    for (std::string_view line : lines) {
        if (ProcessTranscriptLine(line, session, sink)) ++processed;
    }

    session.transcriptOffset += consumed;
    session.transcriptSeen = true;
    return processed;
}

void TranscriptReader::ReadHeadMetadata(SessionState& session) {
    if (session.transcriptPath.empty()) return;

    Handle file(::CreateFileW(session.transcriptPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return;

    // Перші рядки транскрипту містять cwd, gitBranch і початковий запит
    // користувача. Цього фрагмента досить, і він не залежить від того,
    // наскільки великим став файл за час роботи.
    constexpr size_t kHeadWindow = 128 * 1024;

    std::string buffer(kHeadWindow, '\0');
    DWORD read = 0;
    if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(kHeadWindow), &read, nullptr)) {
        return;
    }
    buffer.resize(read);
    if (buffer.empty()) return;

    std::vector<std::string_view> lines;
    size_t consumed = 0;
    SplitLines(buffer, lines, consumed);

    // Приймач подій навмисно порожній: з голови файлу беруться лише
    // метадані, а події звідти застарілі й користувачу не потрібні.
    const EventSink discard = [](Event&&) {};

    // Голова файлу читається в окрему копію стану, і з неї переноситься лише те,
    // що справді описує задачу: тека, проєкт, гілка, назва. Решта — стан,
    // відлік відповіді, контекст, черга повідомлень — у давніх рядках застаріла.
    // Колись звідси витікав початок відповіді з першого запиту сесії, і хвіст,
    // що починався посеред роботи, успадковував його: «відповідь за 103 год».
    SessionState scratch;
    scratch.sessionId = session.sessionId;
    scratch.cwd = session.cwd;
    scratch.project = session.project;
    scratch.gitBranch = session.gitBranch;
    scratch.title = session.title;
    scratch.titleSource = session.titleSource;

    for (std::string_view line : lines) {
        ProcessTranscriptLine(line, scratch, discard);

        // Щойно знайдено явно задану назву — далі шукати немає сенсу.
        if (scratch.titleSource == TitleSource::Custom && !scratch.cwd.empty()) break;
    }

    session.cwd = scratch.cwd;
    session.project = scratch.project;
    session.gitBranch = scratch.gitBranch;
    session.title = scratch.title;
    session.titleSource = scratch.titleSource;
}

size_t TranscriptReader::ReadTail(SessionState& session, size_t maxLines, const EventSink& sink) {
    if (session.transcriptPath.empty()) return 0;

    // Спершу метадані з початку файлу, потім свіжі події з кінця.
    ReadHeadMetadata(session);

    Handle file(::CreateFileW(session.transcriptPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return 0;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) return 0;
    const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);

    // Читаємо лише останній фрагмент файлу. Транскрипти сягають десятків
    // мегабайтів, а для відновлення стану потрібні лише останні події.
    constexpr uint64_t kTailWindow = 256 * 1024;
    const uint64_t start = fileSize > kTailWindow ? fileSize - kTailWindow : 0;

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(start);
    if (!::SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) return 0;

    std::string buffer(static_cast<size_t>(fileSize - start), '\0');
    DWORD read = 0;
    if (!buffer.empty() &&
        !::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        return 0;
    }
    buffer.resize(read);

    std::vector<std::string_view> lines;
    size_t consumed = 0;
    SplitLines(buffer, lines, consumed);

    // Якщо читання почалося з середини файлу, перший рядок майже напевно
    // обрізаний — відкидаємо його.
    if (start > 0 && !lines.empty()) lines.erase(lines.begin());

    // Обробляємо лише останні maxLines рядків.
    size_t from = lines.size() > maxLines ? lines.size() - maxLines : 0;

    size_t processed = 0;
    for (size_t i = from; i < lines.size(); ++i) {
        if (ProcessTranscriptLine(lines[i], session, sink)) ++processed;
    }
    // Хвіст — уже записана частина файлу: притримувати тут нічого.
    FlushHeldTurnEnd(session, sink);

    // Хвіст почався посеред відповіді — її початок лежить раніше. Без нього
    // таймер на телефоні зникав після кожного перезапуску Bridge, хоча Claude
    // і далі працював. Шукаємо запит, з якого відповідь почалась, назад
    // від першого прочитаного рядка.
    if (IsTurnState(session.state) && !session.turnStartKnown && session.lastUpdateMs != 0) {
        const uint64_t now = NowUnixMs();
        // Відповідь, у якій понад пів години не було жодного запису, насправді
        // обірвалась — рахувати її від давнього запиту було б вигадкою.
        const bool fresh = now >= session.lastUpdateMs &&
                           now - session.lastUpdateMs <= kAbandonedTurnMs;
        if (fresh) {
            const uint64_t scanEnd =
                from < lines.size()
                    ? start + static_cast<uint64_t>(lines[from].data() - buffer.data())
                    : fileSize;
            const uint64_t turnStart = FindTurnStartBefore(file.get(), scanEnd);
            if (turnStart != 0 && turnStart <= session.lastUpdateMs) {
                session.turnStartedAtMs = turnStart;
                session.turnStartKnown = true;
            }
        }
    }

    session.transcriptOffset = fileSize;
    session.transcriptSeen = true;
    return processed;
}

}  // namespace cm
