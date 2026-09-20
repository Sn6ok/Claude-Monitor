// nodes.h — вузли (nodes): розширення, які додають у застосунок свої дані.
//
// Ідея як у модів до гри: поклали каталог із вузлом у папку — він працює,
// прибрали — його немає. Жодної реєстрації, збірки чи правки коду.
//
//   %USERPROFILE%\.claude-monitor\nodes\
//   └── system-info\
//       ├── node.json     опис: назва, що запускати, як часто
//       └── run.ps1       будь-яка програма, яка друкує JSON у stdout
//
// БЕЗПЕКА. Вузол — це чужий код, який виконується на вашому комп'ютері
// з вашими правами. Тому:
//   • вузли запускаються лише з вашої папки — Bridge нічого не завантажує;
//   • кожен запуск обмежений часом і розміром виводу, зависання виключене;
//   • вузол не отримує ні ключів, ні коду pairing, ні вмісту ваших сесій:
//     він нічого не знає про Claude Code і не може нічого надіслати телефону
//     повз Bridge;
//   • усе, що вузол надрукував, проходить те саме маскування секретів, що й
//     звичайні події (Частина 3 §5 Master Prompt);
//   • ставте лише ті вузли, яким довіряєте, — як і моди до гри.

#pragma once

#include "common.h"

#include <atomic>
#include <string>
#include <vector>

namespace cm {

// ── Межі (Частина 4 §21: усе обмежене згори) ─────────────────────────────────

inline constexpr size_t kMaxNodes = 16;            ///< вузлів у папці
inline constexpr size_t kMaxNodeLines = 12;        ///< рядків у картці вузла
inline constexpr size_t kMaxNodeLabel = 48;        ///< підпис рядка
inline constexpr size_t kMaxNodeValue = 160;       ///< значення рядка
inline constexpr size_t kMaxNodeText = 512;        ///< вільний текст картки
inline constexpr size_t kMaxNodeOutputBytes = 64 * 1024;

inline constexpr uint32_t kMinNodeIntervalSec = 5;
inline constexpr uint32_t kDefaultNodeIntervalSec = 30;
inline constexpr uint32_t kMaxNodeTimeoutSec = 30;
inline constexpr uint32_t kDefaultNodeTimeoutSec = 10;

/// Причина, з якої даних немає. Код, а не текст: застосунок скаже це
/// своєю мовою, а не покаже чуже повідомлення.
enum class NodeError : uint8_t {
    None = 0,
    StartFailed,   ///< не вдалося запустити
    Timeout,       ///< не встиг за відведений час
    ExitCode,      ///< завершився з помилкою
    BadOutput,     ///< надрукував не той JSON
    TooLarge,      ///< надрукував забагато
};

const char* ToString(NodeError error);

/// Опис вузла з node.json.
struct NodeManifest {
    std::string id;           ///< з імені каталогу, якщо не задано явно
    std::string name;         ///< що показати в застосунку
    std::string description;
    std::string version;
    std::string run;          ///< що запускати; шлях — відносно каталогу вузла
    uint32_t    intervalSec = kDefaultNodeIntervalSec;
    uint32_t    timeoutSec = kDefaultNodeTimeoutSec;
    bool        enabled = true;

    bool        valid = false;
    std::string problem;      ///< чому маніфест непридатний (для журналу)
};

struct NodeLine {
    std::string label;
    std::string value;
};

/// Те, що вузол показує в застосунку.
struct NodeResult {
    std::string           id;
    std::string           name;
    std::string           status;        ///< ok | warn | error
    std::vector<NodeLine> lines;
    std::string           text;
    NodeError             error = NodeError::None;
    uint64_t              updatedAtMs = 0;
};

/// Розбирає node.json. Нічого не вигадує: чого в маніфесті немає — того немає.
/// @param folderId ім'я каталогу вузла — запасний ідентифікатор і назва
NodeManifest ParseNodeManifest(std::string_view json, std::string_view folderId);

/// Розбирає те, що вузол надрукував у stdout.
///
/// Очікується об'єкт JSON:
///   { "status":"ok", "lines":[{"label":"CPU","value":"12%"}], "text":"…" }
///
/// Зайве обрізається за межами вище, секрети маскуються.
NodeResult ParseNodeOutput(const NodeManifest& manifest, std::string_view output,
                           uint64_t nowMs);

/// Результат вузла, який не вдалося виконати.
NodeResult MakeNodeFailure(const NodeManifest& manifest, NodeError error, uint64_t nowMs);

/// Повний командний рядок для запуску вузла.
///
/// Зручність, заради якої вузол пишеться одним файлом: `run.ps1` само
/// запускається через PowerShell, `run.js` — через Node, `run.py` — через
/// Python, `run.exe` — напряму. Рядок із пробілами вважається готовою
/// командою і не змінюється.
std::wstring BuildNodeCommandLine(const std::string& run);

/// Запускає вузли за розкладом і тримає їхні останні результати.
///
/// Живе у власному потоці: повільний вузол не має права затримати ні читання
/// транскриптів, ні мережу.
class NodeRunner {
public:
    explicit NodeRunner(std::wstring directory);
    ~NodeRunner();

    NodeRunner(const NodeRunner&) = delete;
    NodeRunner& operator=(const NodeRunner&) = delete;

    /// Перечитує каталог вузлів. Викликається на старті й коли папка змінилась.
    /// @returns скільки придатних вузлів знайдено
    size_t Reload();

    void Start();
    void Stop();

    /// Копія останніх результатів — для знімка стану.
    std::vector<NodeResult> results() const;

    size_t count() const;

private:
    struct Entry {
        NodeManifest manifest;
        std::wstring directory;
        uint64_t     nextRunMonoMs = 0;
    };

    static DWORD WINAPI ThreadEntry(LPVOID parameter);
    void Loop();

    /// Виконує вузол і повертає його результат. Блокується не довше timeoutSec.
    NodeResult RunOne(const Entry& entry);

    std::wstring directory_;

    mutable Lock      lock_;
    std::vector<Entry>      entries_;   ///< під lock_
    std::vector<NodeResult> results_;   ///< під lock_

    Handle            thread_;
    Handle            wakeEvent_;
    std::atomic<bool> running_{false};
    uint64_t          lastScanMonoMs_ = 0;
};

}  // namespace cm
