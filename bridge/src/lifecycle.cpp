// lifecycle.cpp — єдиний екземпляр і прив'язка до процесу Claude Desktop.

#include "lifecycle.h"
#include "logging.h"

#include <tlhelp32.h>

#include <algorithm>

namespace cm {

namespace {

/// Ім'я м'ютекса. Простір Local\ обмежує його сеансом користувача:
/// у кожного користувача свій Claude Desktop і свій Bridge.
constexpr wchar_t kMutexName[] = L"Local\\ClaudeMonitorBridge.v1";

/// Ознаки встановленого Claude Desktop у шляху до виконуваного файлу.
/// Перевіряються обидві: застосунок може бути встановлений як пакет
/// із Microsoft Store або звичайним інсталятором.
constexpr wchar_t kMarkerStore[]   = L"\\WindowsApps\\Claude";
constexpr wchar_t kMarkerProgram[] = L"\\AnthropicClaude\\";
constexpr wchar_t kExeName[]       = L"claude.exe";

std::wstring GetProcessImagePath(uint32_t pid) {
    Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return {};

    wchar_t buffer[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buffer));

    // QueryFullProcessImageNameW працює з обмеженими правами — на відміну
    // від GetModuleFileNameEx, який потребує PROCESS_VM_READ.
    if (!::QueryFullProcessImageNameW(process.get(), 0, buffer, &size)) return {};
    return std::wstring(buffer, size);
}

/// Час створення процесу в мілісекундах Unix. Нуль, якщо недоступний.
uint64_t GetProcessStartMs(uint32_t pid) {
    Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return 0;

    FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
    if (!::GetProcessTimes(process.get(), &creation, &exitTime, &kernelTime, &userTime)) {
        return 0;
    }

    ULARGE_INTEGER value{};
    value.LowPart  = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;

    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (value.QuadPart < kEpochDiff100ns) return 0;
    return (value.QuadPart - kEpochDiff100ns) / 10000ULL;
}

bool LooksLikeClaudeDesktop(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;
    if (!EndsWithNoCase(imagePath, kExeName)) return false;

    // Виконуваний файл самого Claude Code лежить в іншому місці
    // (AppData\Roaming\Claude\claude-code\<версія>\claude.exe) і кореневим
    // процесом Claude Desktop не є (docs/research.md §2).
    if (ContainsNoCase(imagePath, L"\\claude-code\\")) return false;

    return ContainsNoCase(imagePath, kMarkerStore) ||
           ContainsNoCase(imagePath, kMarkerProgram);
}

}  // namespace

// ── SingleInstance ───────────────────────────────────────────────────────────

SingleInstance::~SingleInstance() {
    // М'ютекс звільняється автоматично при закритті дескриптора, зокрема
    // й у разі аварійного завершення процесу: Windows закриє дескриптор сама.
    // Тому «зависла» блокування після падіння Bridge неможлива.
}

bool SingleInstance::Acquire() {
    mutex_.reset(::CreateMutexW(nullptr, TRUE, kMutexName));

    if (!mutex_) {
        Log().Errorf("не вдалося створити м'ютекс єдиного екземпляра (код %lu)", ::GetLastError());
        return false;
    }

    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        alreadyRunning_ = true;
        mutex_.reset();
        return false;
    }

    alreadyRunning_ = false;
    return true;
}

// ── DesktopMonitor ───────────────────────────────────────────────────────────

ClaudeDesktopInfo DesktopMonitor::Find() {
    ClaudeDesktopInfo result;

    Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        Log().Warnf("не вдалося отримати перелік процесів (код %lu)", ::GetLastError());
        return result;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) return result;

    // Усі процеси застосунку називаються claude.exe, тож ім'я нічого не
    // вирішує (docs/research.md §1). Збираємо кандидатів і визначаємо
    // кореневий за структурою дерева процесів.
    //
    // Свідомо НЕ читаємо командні рядки чужих процесів: це вимагало б
    // доступу до їхньої пам'яті, чого Master Prompt (Частина 5 §11) велить
    // уникати. Дерево процесів дає ту саму відповідь без такого доступу.
    struct Candidate {
        uint32_t pid = 0;
        uint32_t parentPid = 0;
        std::wstring imagePath;
        uint64_t startedAtMs = 0;
    };
    std::vector<Candidate> candidates;

    do {
        if (::CompareStringOrdinal(entry.szExeFile, -1, kExeName, -1, TRUE) != CSTR_EQUAL) {
            continue;
        }

        const std::wstring imagePath = GetProcessImagePath(entry.th32ProcessID);
        if (!LooksLikeClaudeDesktop(imagePath)) continue;

        Candidate candidate;
        candidate.pid         = entry.th32ProcessID;
        candidate.parentPid   = entry.th32ParentProcessID;
        candidate.imagePath   = imagePath;
        candidate.startedAtMs = GetProcessStartMs(entry.th32ProcessID);
        candidates.push_back(std::move(candidate));

        // Захист від нескінченного накопичення, якщо в системі раптом
        // виявиться аномально багато відповідних процесів.
        if (candidates.size() >= 128) break;

    } while (::Process32NextW(snapshot.get(), &entry));

    if (candidates.empty()) return result;

    // Кореневий процес — той, чий батько не належить до тієї самої групи.
    // Дочірні процеси Chromium завжди породжені кореневим, тож їхні батьки
    // у списку є, а батько кореневого (провідник або служба) — ні.
    std::vector<const Candidate*> roots;
    for (const Candidate& candidate : candidates) {
        bool parentIsCandidate = false;
        for (const Candidate& other : candidates) {
            if (other.pid == candidate.parentPid) { parentIsCandidate = true; break; }
        }
        if (!parentIsCandidate) roots.push_back(&candidate);
    }

    if (roots.empty()) {
        // Такого не має статися, але як запобіжник беремо найстаріший процес:
        // кореневий завжди створюється першим.
        Log().Debug("кореневий процес Claude Desktop за деревом не визначено, беру найстаріший");
        const Candidate* oldest = &candidates.front();
        for (const Candidate& candidate : candidates) {
            if (candidate.startedAtMs != 0 &&
                (oldest->startedAtMs == 0 || candidate.startedAtMs < oldest->startedAtMs)) {
                oldest = &candidate;
            }
        }
        roots.push_back(oldest);
    }

    // Якщо коренів кілька (наприклад, після оновлення застосунку лишився
    // старий процес), беремо найстаріший: саме він тримає сесію.
    const Candidate* chosen = roots.front();
    for (const Candidate* candidate : roots) {
        if (candidate->startedAtMs != 0 &&
            (chosen->startedAtMs == 0 || candidate->startedAtMs < chosen->startedAtMs)) {
            chosen = candidate;
        }
    }

    result.pid = chosen->pid;
    result.executablePath = chosen->imagePath;
    result.found = true;
    return result;
}

Handle DesktopMonitor::OpenForWait(uint32_t pid) {
    if (pid == 0) return Handle();

    // SYNCHRONIZE дозволяє чекати завершення процесу через
    // WaitForSingleObject — це і є механізм із нульовим споживанням CPU
    // (Частина 5 §7, §31 Master Prompt).
    return Handle(::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
}

bool DesktopMonitor::IsAlive(HANDLE process) {
    if (!process) return false;
    return ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

}  // namespace cm
