// sessions.cpp — сканування реєстру сесій Claude Code.

#include "sessions.h"
#include "json.h"

#include <psapi.h>

namespace cm {

SessionScanner::SessionScanner() {
    claudeHome_ = ClaudeHomeDir();
    if (claudeHome_.empty()) return;

    sessionsDir_ = claudeHome_ + L"\\sessions";
    projectsDir_ = claudeHome_ + L"\\projects";

    // Якщо каталогу сесій немає, Claude Code цієї версії його не веде.
    // Це не аварія: Bridge працюватиме з тими сесіями, про які дізнається
    // через hook (docs/architecture.md §5.1, «чесне обмеження»).
    const DWORD attributes = ::GetFileAttributesW(sessionsDir_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        sessionsDir_.clear();
    }
}

std::wstring SessionScanner::SlugForPath(std::string_view cwd) {
    // Правило виведено зі спостережених імен каталогів:
    //   "D:\Wall\For Hacker\Claude Monito" -> "D--Wall-For-Hacker-Claude-Monito"
    // Кожен символ поза набором [A-Za-z0-9-] стає дефісом, включно з
    // не-ASCII символами (кожен байт окремо).
    const std::wstring wide = Utf8ToUtf16(cwd);

    std::wstring slug;
    slug.reserve(wide.size() + 4);

    for (wchar_t c : wide) {
        const bool keep = (c >= L'a' && c <= L'z') ||
                          (c >= L'A' && c <= L'Z') ||
                          (c >= L'0' && c <= L'9') ||
                          c == L'-';
        slug += keep ? c : L'-';
    }
    return slug;
}

bool SessionScanner::IsSessionProcessAlive(uint32_t pid, uint64_t startedAtMs) {
    if (pid == 0) return false;

    // Мінімальні права: лише запит базової інформації. Bridge не має
    // потребувати SeDebugPrivilege чи інших привілеїв (Частина 5 §25).
    Handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return false;

    DWORD exitCode = 0;
    if (!::GetExitCodeProcess(process.get(), &exitCode)) return false;
    if (exitCode != STILL_ACTIVE) return false;

    // Номери процесів переви́користовуються операційною системою. Якщо файл
    // сесії залишився від давно померлого процесу, а система видала той самий
    // PID новому процесу, ми б помилково вважали сесію живою. Тому звіряємо
    // час створення процесу з часом старту сесії.
    if (startedAtMs != 0) {
        FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
        if (::GetProcessTimes(process.get(), &creation, &exitTime, &kernelTime, &userTime)) {
            ULARGE_INTEGER value{};
            value.LowPart  = creation.dwLowDateTime;
            value.HighPart = creation.dwHighDateTime;

            constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
            if (value.QuadPart > kEpochDiff100ns) {
                const uint64_t processStartMs = (value.QuadPart - kEpochDiff100ns) / 10000ULL;

                // Процес не міг стартувати помітно пізніше за сесію.
                // Допуск у 60 секунд покриває розбіжності округлення.
                if (processStartMs > startedAtMs + 60'000) return false;
            }
        }
    }

    return true;
}

bool SessionScanner::ParseSessionFile(const std::wstring& path, DiscoveredSession& out) const {
    std::string content;
    if (!ReadWholeFile(path, content, 64 * 1024)) return false;

    json::Value root;
    if (!json::Parse(content, root) || !root.isObject()) return false;

    out.sessionId = root["sessionId"].asStringOr("");
    if (out.sessionId.empty()) return false;

    const int64_t pid = root["pid"].asInt(0);
    if (pid <= 0 || pid > 0xFFFFFFFF) return false;
    out.pid = static_cast<uint32_t>(pid);

    out.cwd         = root["cwd"].asStringOr("");
    out.name        = root["name"].asStringOr("");
    out.version     = root["version"].asStringOr("");
    out.entrypoint  = root["entrypoint"].asStringOr("");
    out.startedAtMs = static_cast<uint64_t>(std::max<int64_t>(0, root["startedAt"].asInt(0)));

    return true;
}

std::wstring SessionScanner::FindTranscript(const std::string& sessionId,
                                            std::string_view cwd) const {
    if (projectsDir_.empty() || sessionId.empty()) return {};

    const std::wstring fileName = Utf8ToUtf16(sessionId) + L".jsonl";

    // Швидкий шлях: обчислене ім'я каталогу.
    if (!cwd.empty()) {
        const std::wstring candidate = projectsDir_ + L"\\" + SlugForPath(cwd) + L"\\" + fileName;
        if (FileExists(candidate)) return candidate;
    }

    // Запасний шлях: перебір каталогів проєктів. Правило формування імені
    // каталогу не документоване, тож покладатися лише на нього не можна.
    // Перебір дешевий: перевіряється існування конкретного файлу, а не вміст.
    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = projectsDir_ + L"\\*";

    Handle search(::FindFirstFileW(pattern.c_str(), &findData));
    if (!search) return {};

    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (findData.cFileName[0] == L'.') continue;

        const std::wstring candidate =
            projectsDir_ + L"\\" + findData.cFileName + L"\\" + fileName;
        if (FileExists(candidate)) {
            ::FindClose(search.release());
            return candidate;
        }
    } while (::FindNextFileW(search.get(), &findData));

    ::FindClose(search.release());
    return {};
}

std::vector<DiscoveredSession> SessionScanner::Scan() const {
    std::vector<DiscoveredSession> result;
    if (sessionsDir_.empty()) return result;

    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = sessionsDir_ + L"\\*.json";

    Handle search(::FindFirstFileW(pattern.c_str(), &findData));
    if (!search) return result;

    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        DiscoveredSession session;
        const std::wstring fullPath = sessionsDir_ + L"\\" + findData.cFileName;
        if (!ParseSessionFile(fullPath, session)) continue;

        // Мертві сесії відкидаються тут: показувати задачу, процес якої
        // уже завершився, було б неправдою (Частина 6 §42 Master Prompt).
        if (!IsSessionProcessAlive(session.pid, session.startedAtMs)) continue;

        session.transcriptPath = FindTranscript(session.sessionId, session.cwd);
        result.push_back(std::move(session));

        // Верхня межа кількості сесій — захист від необмеженого росту
        // пам'яті, якщо в системі раптом виявиться аномально багато файлів.
        if (result.size() >= kMaxSessions) break;

    } while (::FindNextFileW(search.get(), &findData));

    ::FindClose(search.release());
    return result;
}

}  // namespace cm
