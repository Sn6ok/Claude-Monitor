// events.h — модель даних Bridge: події, стани, сесії.
//
// Ключове архітектурне рішення: КОЖНА подія несе ідентифікатор сесії.
// Поняття «поточної сесії» в моделі відсутнє свідомо — саме це технічно
// унеможливлює приписування подій однієї задачі іншій
// (docs/architecture.md §5.1).

#pragma once

#include "common.h"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace cm {

// ── Стан Claude Code ─────────────────────────────────────────────────────────
//
// Кожен стан має спостережуваний доказ у транскрипті. Стану «здогадка»
// не існує: за браку даних застосовується Unknown
// (Частина 5 §12, Частина 6 §42 Master Prompt).

enum class ClaudeState : uint8_t {
    Unknown = 0,   ///< дані недоступні або формат не розпізнано
    Starting,      ///< сесія зареєстрована, транскрипт ще порожній
    Idle,          ///< відповідь завершено, очікує наступного запиту
    Working,       ///< виконує інструмент або формує відповідь
    Waiting,       ///< очікує дії користувача (дозвіл, відповідь)
    Error,         ///< остання операція завершилась помилкою
    Limited,       ///< вичерпано ліміт використання; далі — лише після скидання
    Finished,      ///< сесія коректно завершена
    Gone,          ///< процес сесії зник без коректного завершення
};

const char* ToString(ClaudeState state);

// ── Види подій ───────────────────────────────────────────────────────────────

enum class EventKind : uint8_t {
    Status = 0,   ///< зміна стану сесії
    Activity,     ///< дія: читання, редагування, команда
    Output,       ///< текстова репліка Claude
    Result,       ///< результат виконання інструмента
    Session,      ///< поява або зникнення сесії
    Prompt,       ///< запит користувача — друга половина розмови
};

const char* ToString(EventKind kind);

/// Нормалізовані дії. Невідомий інструмент дає Other — не вигадану назву.
enum class ActivityAction : uint8_t {
    Other = 0,
    ReadFile,
    EditFile,
    WriteFile,
    RunCommand,
    Search,
    WebFetch,
    Task,
    Subagent,
};

const char* ToString(ActivityAction action);

/// Пріоритет визначає, що витісняється першим при переповненні черги
/// (docs/protocol.md §8). Зміни стану не витісняються ніколи.
enum class Priority : uint8_t {
    Low = 0,
    Medium,
    High,
};

// ── Подія ────────────────────────────────────────────────────────────────────

struct Event {
    std::string    sessionId;              ///< обов'язкове поле, порожнім не буває
    uint64_t       sequence = 0;           ///< глобальний монотонний лічильник
    uint64_t       timestampMs = 0;
    EventKind      kind = EventKind::Output;
    Priority       priority = Priority::Medium;

    // Заповнюються залежно від kind.
    ClaudeState    state = ClaudeState::Unknown;   // для Status
    ActivityAction action = ActivityAction::Other; // для Activity
    std::string    target;                         // шлях або команда
    std::string    text;                           // текст, уже обрізаний і очищений
    bool           isError = false;                // для Result

    /// Скільки зображень було в запиті. Самі зображення на телефон
    /// не передаються — лише їхня кількість.
    uint16_t       imageCount = 0;

    // Довгий текст ділиться на частини, бо кадр обмежений за розміром.
    // Частини однієї репліки мають спільний groupId — за ним застосунок
    // склеює їх назад в одне повідомлення. partCount == 1 означає, що
    // ділити не довелося.
    uint64_t       groupId = 0;
    uint16_t       part = 0;
    uint16_t       partCount = 1;

    // Час відповіді — лише для Status. turnStartedAtMs: коли почалася поточна
    // відповідь (іде, поки вона триває). turnMs: скільки вона тривала (іде
    // один раз, у події про її завершення). 0 — невідомо.
    uint64_t       turnStartedAtMs = 0;
    uint64_t       turnMs = 0;

    /// Наближений розмір у пам'яті — потрібен для контролю росту черги.
    size_t approximateBytes() const {
        return sizeof(Event) + sessionId.size() + target.size() + text.size();
    }
};

/// Приймач подій. Усі виробники подій (читач транскрипту, канал hook,
/// менеджер сесій) віддають результат саме через нього — це тримає
/// напрямок залежностей одностороннім.
using EventSink = std::function<void(Event&&)>;

// ── Джерело назви задачі ─────────────────────────────────────────────────────
//
// Передається на телефон, щоб UI міг чесно показати, звідки взято назву,
// і не видавав похідну назву за задану людиною.

enum class TitleSource : uint8_t {
    None = 0,
    Custom,    ///< custom-title з транскрипту — задано явно
    Derived,   ///< name з реєстру сесій — автоматична назва Claude Code
    Prompt,    ///< перший рядок запиту користувача
    Path,      ///< лише назва робочого каталогу
};

const char* ToString(TitleSource source);

// ── Стан однієї сесії ────────────────────────────────────────────────────────

struct SessionState {
    std::string  sessionId;
    uint32_t     pid = 0;

    std::string  title;
    TitleSource  titleSource = TitleSource::None;
    std::string  project;      ///< назва останнього каталогу з cwd
    std::string  cwd;
    std::string  gitBranch;

    ClaudeState  state = ClaudeState::Unknown;

    ActivityAction currentAction = ActivityAction::Other;
    std::string    currentTarget;
    bool           hasActivity = false;

    uint64_t startedAtMs = 0;      ///< з реєстру сесій
    uint64_t lastUpdateMs = 0;     ///< час останньої обробленої події
    uint64_t lastSequence = 0;

    /// Скільки токенів займає контекст розмови просто зараз.
    ///
    /// Береться з поля usage останньої відповіді: сума прочитаного з кешу,
    /// створеного кешу та звичайного вводу. Це саме те число, яке Claude Code
    /// показує у себе як «context window».
    uint64_t contextTokens = 0;

    /// Модель поточної сесії — за нею визначається межа контексту.
    std::string model;

    /// Коли почалася поточна відповідь: від вашого запиту. 0 — не триває.
    uint64_t turnStartedAtMs = 0;

    /// Чи бачив Bridge сам початок відповіді. Якщо він запустився посеред
    /// неї, справжній початок невідомий — і тривалість не показується:
    /// вигаданий час гірший за відсутній (Частина 5 §12 Master Prompt).
    bool turnStartKnown = false;

    /// Скільки тривала остання завершена відповідь. 0 — ще не було.
    uint64_t lastTurnMs = 0;

    /// Коли скидається вичерпаний ліміт (мс Unix). 0 — ліміту немає або його
    /// час невідомий. Коли момент настає, задача сама повертається в очікування.
    uint64_t limitResetAtMs = 0;

    /// Повідомлення, які ви надіслали, поки Claude працював, і які ще чекають
    /// у черзі Claude Code. Поки така черга не порожня, кінець відповіді —
    /// не кінець розмови: Claude одразу візьметься за наступне повідомлення.
    uint32_t queuedPrompts = 0;
    uint64_t lastEnqueueMs = 0;

    /// Кінець відповіді відкладено, бо в черзі чекає ваше повідомлення.
    bool idleDeferred = false;

    /// Подія кінця відповіді, притримана до останнього блоку репліки.
    ///
    /// Claude Code пише кожен блок репліки окремим рядком, і ознаку кінця ходу
    /// несе вже перший із них (зазвичай thinking). Оголошена одразу, позначка
    /// «ГОТОВО» ставала в стрічці перед останнім текстом Claude.
    std::vector<Event> heldTurnEnd;
    std::string heldTurnEndMessageId;

    /// Відбитки вже показаних повідомлень, чий повтор ще має з'явитися
    /// в транскрипті. Потрібні, щоб одне повідомлення не показалось двічі.
    std::deque<size_t> expectedPromptDuplicates;

    /// Останні події цієї сесії — для snapshot. Кільцевий буфер,
    /// обмежений kMaxSnapshotEvents.
    std::deque<Event> recentEvents;

    // Внутрішній стан читача транскрипту.
    std::wstring transcriptPath;
    uint64_t     transcriptOffset = 0;
    bool         transcriptSeen = false;

    /// Незавершений виклик інструмента: є tool_use, але ще немає tool_result.
    /// Саме це є доказом стану Working.
    bool     pendingToolUse = false;
    uint64_t pendingSinceMs = 0;

    bool alive = true;

    void pushRecent(const Event& event) {
        recentEvents.push_back(event);
        while (recentEvents.size() > kMaxSnapshotEvents) recentEvents.pop_front();
    }
};

// ── Загальний підсумок ───────────────────────────────────────────────────────

struct Summary {
    uint32_t active = 0;
    uint32_t working = 0;
    uint32_t waiting = 0;
    uint32_t idle = 0;
    uint32_t finished = 0;
    uint32_t error = 0;
};

}  // namespace cm
