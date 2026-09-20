// nodes.cpp — запуск вузлів і розбір їхніх даних.

#include "nodes.h"
#include "json.h"
#include "logging.h"
#include "redact.h"

#include <algorithm>

namespace cm {

namespace {

/// Як часто перечитувати папку вузлів: додали чи прибрали каталог.
constexpr uint64_t kNodeScanIntervalMs = 10'000;

/// Крок циклу вузлів. Дрібніше не потрібно: найменший інтервал — 5 секунд.
constexpr uint32_t kNodeTickMs = 1'000;

std::string Clean(std::string_view raw, size_t limit) {
    if (raw.empty()) return {};
    std::string text = SanitizeText(raw);
    if (text.empty()) return {};
    return TruncateUtf8(RedactSecrets(text), limit);
}

std::string CleanMultiline(std::string_view raw, size_t limit) {
    if (raw.empty()) return {};
    std::string text = SanitizeMultiline(raw);
    if (text.empty()) return {};
    return TruncateUtf8(RedactSecrets(text), limit);
}

/// Ідентифікатор має бути придатним для показу й порівняння: лише букви,
/// цифри, дефіс і підкреслення.
std::string CleanId(std::string_view raw) {
    std::string out;
    for (char c : raw) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out += c;
        if (out.size() >= 48) break;
    }
    return out;
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = text[text.size() - suffix.size() + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

const char* ToString(NodeError error) {
    switch (error) {
        case NodeError::None:        return "";
        case NodeError::StartFailed: return "start_failed";
        case NodeError::Timeout:     return "timeout";
        case NodeError::ExitCode:    return "exit_code";
        case NodeError::BadOutput:   return "bad_output";
        case NodeError::TooLarge:    return "too_large";
    }
    return "";
}

// ── Маніфест ─────────────────────────────────────────────────────────────────

NodeManifest ParseNodeManifest(std::string_view text, std::string_view folderId) {
    NodeManifest manifest;
    manifest.id = CleanId(folderId);
    manifest.name = Clean(folderId, kMaxNodeLabel);

    json::Value root;
    if (!json::Parse(text, root) || !root.isObject()) {
        manifest.problem = "node.json не читається";
        return manifest;
    }

    const std::string id = CleanId(root["id"].asStringOr(""));
    if (!id.empty()) manifest.id = id;
    if (manifest.id.empty()) {
        manifest.problem = "порожній ідентифікатор";
        return manifest;
    }

    const std::string name = Clean(root["name"].asStringOr(""), kMaxNodeLabel);
    if (!name.empty()) manifest.name = name;

    manifest.description = Clean(root["description"].asStringOr(""), kMaxNodeValue);
    manifest.version = Clean(root["version"].asStringOr(""), 24);
    manifest.run = Clean(root["run"].asStringOr(""), 512);
    manifest.enabled = root["enabled"].asBool(true);

    if (manifest.run.empty()) {
        manifest.problem = "не вказано, що запускати (поле run)";
        return manifest;
    }

    const int64_t interval = root["intervalSec"].asInt(kDefaultNodeIntervalSec);
    manifest.intervalSec = static_cast<uint32_t>(
        std::clamp<int64_t>(interval, kMinNodeIntervalSec, 24 * 60 * 60));

    const int64_t timeout = root["timeoutSec"].asInt(kDefaultNodeTimeoutSec);
    manifest.timeoutSec = static_cast<uint32_t>(std::clamp<int64_t>(timeout, 1, kMaxNodeTimeoutSec));

    manifest.valid = true;
    return manifest;
}

// ── Вивід вузла ──────────────────────────────────────────────────────────────

NodeResult MakeNodeFailure(const NodeManifest& manifest, NodeError error, uint64_t nowMs) {
    NodeResult result;
    result.id = manifest.id;
    result.name = manifest.name.empty() ? manifest.id : manifest.name;
    result.status = "error";
    result.error = error;
    result.updatedAtMs = nowMs != 0 ? nowMs : NowUnixMs();
    return result;
}

NodeResult ParseNodeOutput(const NodeManifest& manifest, std::string_view output,
                           uint64_t nowMs) {
    if (output.size() > kMaxNodeOutputBytes) {
        return MakeNodeFailure(manifest, NodeError::TooLarge, nowMs);
    }

    json::Value root;
    if (!json::Parse(output, root) || !root.isObject()) {
        return MakeNodeFailure(manifest, NodeError::BadOutput, nowMs);
    }

    NodeResult result;
    result.id = manifest.id;
    result.updatedAtMs = nowMs != 0 ? nowMs : NowUnixMs();

    // Назву вузол може уточнити, але не вигадати: якщо не сказав — з маніфесту.
    const std::string title = Clean(root["title"].asStringOr(""), kMaxNodeLabel);
    result.name = !title.empty() ? title
                                 : (manifest.name.empty() ? manifest.id : manifest.name);

    const std::string status = Clean(root["status"].asStringOr("ok"), 8);
    result.status = (status == "warn" || status == "error") ? status : "ok";

    const json::Value& lines = root["lines"];
    if (lines.isArray()) {
        for (const json::Value& item : lines.asArray()) {
            if (result.lines.size() >= kMaxNodeLines) break;
            if (!item.isObject()) continue;

            NodeLine line;
            line.label = Clean(item["label"].asStringOr(""), kMaxNodeLabel);
            line.value = Clean(item["value"].asStringOr(""), kMaxNodeValue);
            if (line.label.empty() && line.value.empty()) continue;
            result.lines.push_back(std::move(line));
        }
    }

    result.text = CleanMultiline(root["text"].asStringOr(""), kMaxNodeText);

    // Вузол, який не сказав нічого, — це не дані, а тиша.
    if (result.lines.empty() && result.text.empty()) {
        return MakeNodeFailure(manifest, NodeError::BadOutput, nowMs);
    }
    return result;
}

// ── Командний рядок ──────────────────────────────────────────────────────────

std::wstring BuildNodeCommandLine(const std::string& run) {
    if (run.empty()) return {};

    // Рядок із пробілами — це вже готова команда з аргументами.
    const bool single = run.find(' ') == std::string::npos;
    if (!single) return Utf8ToUtf16(run);

    const std::wstring file = Utf8ToUtf16(run);
    if (EndsWithNoCase(run, ".ps1")) {
        return L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + file + L"\"";
    }
    if (EndsWithNoCase(run, ".js") || EndsWithNoCase(run, ".mjs")) {
        return L"node \"" + file + L"\"";
    }
    if (EndsWithNoCase(run, ".py")) {
        return L"python \"" + file + L"\"";
    }
    if (EndsWithNoCase(run, ".cmd") || EndsWithNoCase(run, ".bat")) {
        return L"cmd.exe /c \"" + file + L"\"";
    }
    return file;
}

// ── Запуск ───────────────────────────────────────────────────────────────────

NodeRunner::NodeRunner(std::wstring directory) : directory_(std::move(directory)) {}

NodeRunner::~NodeRunner() {
    Stop();
}

size_t NodeRunner::Reload() {
    std::vector<Entry> found;

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory_ + L"\\*";
    Handle search(::FindFirstFileW(pattern.c_str(), &data));

    if (search) {
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring folder = data.cFileName;
            if (folder == L"." || folder == L"..") continue;
            if (found.size() >= kMaxNodes) break;

            Entry entry;
            entry.directory = directory_ + L"\\" + folder;

            std::string manifestText;
            if (!ReadWholeFile(entry.directory + L"\\node.json", manifestText, 64 * 1024)) {
                Log().Debugf("вузол %s: немає node.json", Utf16ToUtf8(folder).c_str());
                continue;
            }

            entry.manifest = ParseNodeManifest(manifestText, Utf16ToUtf8(folder));
            if (!entry.manifest.valid) {
                Log().Warnf("вузол %s не підключено: %s", Utf16ToUtf8(folder).c_str(),
                            entry.manifest.problem.c_str());
                continue;
            }
            if (!entry.manifest.enabled) {
                Log().Debugf("вузол %s вимкнено в node.json", entry.manifest.id.c_str());
                continue;
            }

            found.push_back(std::move(entry));
        } while (::FindNextFileW(search.get(), &data));

        // Дескриптор пошуку закривається саме FindClose, а не CloseHandle.
        ::FindClose(search.release());
    }

    Guard guard(lock_);

    // Уже відомі вузли зберігають свій розклад і останній результат:
    // перечитування папки не має скидати все наново.
    for (Entry& entry : found) {
        for (const Entry& old : entries_) {
            if (old.manifest.id == entry.manifest.id) entry.nextRunMonoMs = old.nextRunMonoMs;
        }
    }

    std::vector<NodeResult> keep;
    for (const NodeResult& result : results_) {
        for (const Entry& entry : found) {
            if (entry.manifest.id == result.id) keep.push_back(result);
        }
    }

    entries_ = std::move(found);
    results_ = std::move(keep);
    return entries_.size();
}

void NodeRunner::Start() {
    if (running_) return;
    if (directory_.empty()) return;

    // Папка створюється завжди: у неї ж користувач і кладе вузли.
    EnsureDirectory(directory_);

    Reload();
    running_ = true;
    wakeEvent_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    thread_.reset(::CreateThread(nullptr, 0, &NodeRunner::ThreadEntry, this, 0, nullptr));
    if (!thread_) {
        running_ = false;
        Log().Warn("не вдалося створити потік вузлів");
    }
}

void NodeRunner::Stop() {
    if (!running_) return;
    running_ = false;
    if (wakeEvent_) ::SetEvent(wakeEvent_.get());

    if (thread_) {
        // Вузол міг зависнути; його власний тайм-аут не більший за 30 секунд,
        // але чекати на нього під час завершення Bridge ми не будемо.
        if (::WaitForSingleObject(thread_.get(), 3000) == WAIT_TIMEOUT) {
            Log().Debug("потік вузлів не завершився вчасно");
        }
        thread_.reset();
    }
    wakeEvent_.reset();
}

std::vector<NodeResult> NodeRunner::results() const {
    Guard guard(lock_);
    return results_;
}

size_t NodeRunner::count() const {
    Guard guard(lock_);
    return entries_.size();
}

DWORD WINAPI NodeRunner::ThreadEntry(LPVOID parameter) {
    static_cast<NodeRunner*>(parameter)->Loop();
    return 0;
}

void NodeRunner::Loop() {
    lastScanMonoMs_ = NowMonotonicMs();

    while (running_) {
        const uint64_t now = NowMonotonicMs();

        // Папку перечитуємо зрідка: додати вузол можна будь-коли, але
        // переглядати каталог щосекунди немає сенсу.
        if (now - lastScanMonoMs_ >= kNodeScanIntervalMs) {
            lastScanMonoMs_ = now;
            Reload();
        }

        // Копія розкладу: сам запуск іде без замка, щоб повільний вузол
        // не тримав знімок стану.
        std::vector<Entry> due;
        {
            Guard guard(lock_);
            for (Entry& entry : entries_) {
                if (entry.nextRunMonoMs > now) continue;
                entry.nextRunMonoMs = now + entry.manifest.intervalSec * 1000ULL;
                due.push_back(entry);
            }
        }

        for (const Entry& entry : due) {
            if (!running_) break;
            NodeResult result = RunOne(entry);

            Guard guard(lock_);
            bool replaced = false;
            for (NodeResult& known : results_) {
                if (known.id != result.id) continue;
                known = std::move(result);
                replaced = true;
                break;
            }
            if (!replaced) results_.push_back(std::move(result));
        }

        ::WaitForSingleObject(wakeEvent_.get(), kNodeTickMs);
    }
}

NodeResult NodeRunner::RunOne(const Entry& entry) {
    const uint64_t startedAt = NowUnixMs();

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!::CreatePipe(&readRaw, &writeRaw, &security, 0)) {
        return MakeNodeFailure(entry.manifest, NodeError::StartFailed, startedAt);
    }
    Handle readEnd(readRaw);
    Handle writeEnd(writeRaw);

    // Батьківський кінець каналу дитина успадковувати не повинна.
    ::SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = writeEnd.get();
    startup.hStdInput = nullptr;

    std::wstring commandLine = BuildNodeCommandLine(entry.manifest.run);
    if (commandLine.empty()) {
        return MakeNodeFailure(entry.manifest, NodeError::StartFailed, startedAt);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
        entry.directory.c_str(), &startup, &process);

    if (!started) {
        Log().Warnf("вузол %s не запустився (код %lu)", entry.manifest.id.c_str(),
                    ::GetLastError());
        return MakeNodeFailure(entry.manifest, NodeError::StartFailed, startedAt);
    }

    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);

    // Свій кінець каналу закриваємо одразу: інакше читання ніколи не побачить
    // кінця файлу, навіть коли дитина завершиться.
    writeEnd.reset();

    const uint64_t deadline = NowMonotonicMs() + entry.manifest.timeoutSec * 1000ULL;
    std::string output;
    bool tooLarge = false;
    char buffer[4096];

    while (true) {
        DWORD available = 0;
        if (::PeekNamedPipe(readEnd.get(), nullptr, 0, nullptr, &available, nullptr) &&
            available > 0) {
            DWORD read = 0;
            if (::ReadFile(readEnd.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                if (output.size() + read > kMaxNodeOutputBytes) {
                    tooLarge = true;
                    break;
                }
                output.append(buffer, read);
                continue;
            }
        }

        const DWORD waited = ::WaitForSingleObject(processHandle.get(), 50);
        if (waited == WAIT_OBJECT_0) {
            // Дочитуємо те, що лишилося в каналі після завершення.
            DWORD read = 0;
            while (::PeekNamedPipe(readEnd.get(), nullptr, 0, nullptr, &available, nullptr) &&
                   available > 0 &&
                   ::ReadFile(readEnd.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                if (output.size() + read > kMaxNodeOutputBytes) {
                    tooLarge = true;
                    break;
                }
                output.append(buffer, read);
            }
            break;
        }

        if (NowMonotonicMs() > deadline) {
            ::TerminateProcess(processHandle.get(), 1);
            ::WaitForSingleObject(processHandle.get(), 1000);
            Log().Warnf("вузол %s не встиг за %u с", entry.manifest.id.c_str(),
                        entry.manifest.timeoutSec);
            return MakeNodeFailure(entry.manifest, NodeError::Timeout, startedAt);
        }

        if (!running_) {
            ::TerminateProcess(processHandle.get(), 1);
            ::WaitForSingleObject(processHandle.get(), 1000);
            return MakeNodeFailure(entry.manifest, NodeError::Timeout, startedAt);
        }
    }

    if (tooLarge) {
        ::TerminateProcess(processHandle.get(), 1);
        ::WaitForSingleObject(processHandle.get(), 1000);
        return MakeNodeFailure(entry.manifest, NodeError::TooLarge, startedAt);
    }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(processHandle.get(), &exitCode);
    if (exitCode != 0) {
        Log().Debugf("вузол %s завершився з кодом %lu", entry.manifest.id.c_str(), exitCode);
        return MakeNodeFailure(entry.manifest, NodeError::ExitCode, startedAt);
    }

    return ParseNodeOutput(entry.manifest, output, startedAt);
}

}  // namespace cm
