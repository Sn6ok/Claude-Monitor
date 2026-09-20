// hookpipe.cpp — named pipe для повідомлень від hook-викликів.

#include "hookpipe.h"
#include "json.h"
#include "logging.h"

#include <cstdio>

namespace cm {

namespace {

/// Простір Local\ обмежує канал сеансом користувача — саме те, що потрібно:
/// hook і Bridge завжди працюють від одного облікового запису.
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\LOCAL\\claude-monitor-bridge.v1";

constexpr DWORD kPipeBufferSize = 8 * 1024;
constexpr size_t kMaxMessageBytes = 64 * 1024;

}  // namespace

std::string ReadStdinAll(size_t maxBytes) {
    std::string out;

    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    if (input == INVALID_HANDLE_VALUE || input == nullptr) return out;

    char buffer[4096];
    DWORD read = 0;

    while (::ReadFile(input, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        if (out.size() + read > maxBytes) {
            // Обрізаємо мовчки: хук усе одно передає короткий JSON,
            // а необмежене читання було б вразливістю.
            out.append(buffer, maxBytes - out.size());
            break;
        }
        out.append(buffer, read);
    }
    return out;
}

bool ParseHookInput(std::string_view json, std::string_view eventName, HookMessage& out) {
    out.event.assign(eventName);

    // Порожній вхід не є помилкою: подія все одно варта передачі,
    // просто без подробиць.
    if (json.empty()) return true;

    json::Value root;
    if (!json::Parse(json, root) || !root.isObject()) {
        Log().Debug("вхідні дані хука не є коректним JSON");
        return true;
    }

    // Імена полів відповідають тому, що Claude Code реально передає хукам
    // (docs/research.md §3.3). Відсутність будь-якого з них не є проблемою.
    out.sessionId  = root["session_id"].asStringOr("");
    out.transcript = root["transcript_path"].asStringOr("");
    out.cwd        = root["cwd"].asStringOr("");

    // Текст сповіщення трапляється під різними іменами залежно від події.
    out.text = root["message"].asStringOr("");
    if (out.text.empty()) out.text = root["notification"].asStringOr("");
    if (out.text.empty()) out.text = root["reason"].asStringOr("");

    out.text = TruncateUtf8(SanitizeText(out.text), kMaxEventText);
    return true;
}

// ── Клієнт ───────────────────────────────────────────────────────────────────

bool SendHookMessage(const HookMessage& message, uint32_t timeoutMs) {
    std::string payload;
    json::Writer writer(payload);
    writer.beginObject();
    writer.field("event", message.event);
    writer.field("session_id", message.sessionId);
    writer.field("transcript", message.transcript);
    writer.field("cwd", message.cwd);
    writer.field("text", message.text);
    writer.endObject();

    if (payload.size() > kMaxMessageBytes) return false;

    // Якщо робочого Bridge немає, канал просто відсутній. Це нормальна
    // ситуація: користувач міг не налаштувати Bridge або зупинити його.
    // Хук у такому разі мовчки завершується, НЕ заважаючи Claude Code.
    if (!::WaitNamedPipeW(kPipeName, timeoutMs)) return false;

    Handle pipe(::CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr));
    if (!pipe) return false;

    DWORD written = 0;
    const BOOL ok = ::WriteFile(pipe.get(), payload.data(),
                                static_cast<DWORD>(payload.size()), &written, nullptr);
    return ok && written == payload.size();
}

// ── Сервер ───────────────────────────────────────────────────────────────────

HookPipeServer::~HookPipeServer() { Stop(); }

bool HookPipeServer::CreateAndListen() {
    pipe_.reset(::CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        0, kPipeBufferSize,
        0, nullptr));

    if (!pipe_) {
        Log().Warnf("не вдалося створити канал для хуків (код %lu)", ::GetLastError());
        return false;
    }

    overlapped_ = {};
    overlapped_.hEvent = event_.get();
    ::ResetEvent(event_.get());

    if (::ConnectNamedPipe(pipe_.get(), &overlapped_)) {
        pending_ = true;
        return true;
    }

    const DWORD error = ::GetLastError();
    if (error == ERROR_IO_PENDING) { pending_ = true; return true; }
    if (error == ERROR_PIPE_CONNECTED) {
        // Клієнт устиг під'єднатися до виклику ConnectNamedPipe.
        ::SetEvent(event_.get());
        pending_ = true;
        return true;
    }

    Log().Warnf("ConnectNamedPipe не вдався (код %lu)", error);
    pipe_.reset();
    return false;
}

bool HookPipeServer::Start(Handler handler) {
    Stop();
    handler_ = std::move(handler);

    event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event_) return false;

    if (!CreateAndListen()) return false;

    Log().Debug("канал для хуків відкрито");
    return true;
}

void HookPipeServer::Consume() {
    if (!pipe_ || !pending_) return;

    DWORD transferred = 0;
    if (!::GetOverlappedResult(pipe_.get(), &overlapped_, &transferred, FALSE)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_IO_INCOMPLETE) return;

        // Клієнт відпав, не завершивши обмін — просто чекаємо наступного.
        pipe_.reset();
        CreateAndListen();
        return;
    }

    // Читаємо повідомлення. Клієнт — короткоживучий процес хука, який
    // одразу після запису завершується, тож читання не блокується надовго.
    std::string payload;
    char buffer[kPipeBufferSize];
    DWORD read = 0;

    while (::ReadFile(pipe_.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        if (payload.size() + read > kMaxMessageBytes) break;
        payload.append(buffer, read);
    }

    ::DisconnectNamedPipe(pipe_.get());
    pipe_.reset();
    pending_ = false;

    if (!payload.empty() && handler_) {
        json::Value root;
        if (json::Parse(payload, root) && root.isObject()) {
            HookMessage message;
            message.event      = root["event"].asStringOr("");
            message.sessionId  = root["session_id"].asStringOr("");
            message.transcript = root["transcript"].asStringOr("");
            message.cwd        = root["cwd"].asStringOr("");
            message.text       = root["text"].asStringOr("");

            if (!message.event.empty()) handler_(message);
        } else {
            Log().Debug("отримано некоректне повідомлення від хука");
        }
    }

    // Готуємось до наступного клієнта.
    CreateAndListen();
}

void HookPipeServer::Stop() {
    if (pipe_) {
        ::CancelIoEx(pipe_.get(), &overlapped_);
        ::DisconnectNamedPipe(pipe_.get());
        pipe_.reset();
    }
    event_.reset();
    pending_ = false;
    handler_ = nullptr;
}

}  // namespace cm
