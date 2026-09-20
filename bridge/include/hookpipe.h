// hookpipe.h — канал між hook-викликами Claude Code та робочим Bridge.
//
// Як це працює. Claude Code запускає `claude-monitor-bridge.exe --hook <подія>`
// і подає JSON на стандартний вхід. Цей короткоживучий процес НЕ виконує
// жодної роботи: він передає дані у named pipe робочого Bridge і одразу
// завершується.
//
// Чому саме так (Частина 5 §2 Master Prompt): Claude Code чекає завершення
// hook-процесу. Якби той підключався до мережі чи розбирав транскрипти,
// Claude Code блокувався б на час цих операцій. Передача кількох сотень
// байтів у локальний канал триває мілісекунди.
//
// Використовуються лише три хуки — ті, що спрацьовують рідко:
// SessionStart, SessionEnd, Notification. Хуки на кожен виклик інструмента
// (PreToolUse, PostToolUse) свідомо не застосовуються: ті самі дані вже є
// в транскрипті, а створення процесу на кожну дію було б помітним
// навантаженням (docs/research.md §3.3).

#pragma once

#include "common.h"
#include "events.h"

#include <functional>

namespace cm {

/// Розібране повідомлення від hook.
struct HookMessage {
    std::string event;       ///< session-start | session-end | notification
    std::string sessionId;
    std::string transcript;  ///< шлях до транскрипту, якщо переданий
    std::string cwd;
    std::string text;        ///< текст сповіщення
};

/// Серверна частина: приймає повідомлення від hook-процесів.
class HookPipeServer {
public:
    using Handler = std::function<void(const HookMessage&)>;

    HookPipeServer() = default;
    ~HookPipeServer();

    HookPipeServer(const HookPipeServer&) = delete;
    HookPipeServer& operator=(const HookPipeServer&) = delete;

    bool Start(Handler handler);
    void Stop();

    /// Дескриптор для очікування в загальному циклі.
    HANDLE waitHandle() const { return event_.get(); }

    /// Обробляє під'єднаного клієнта та ставить канал на нове очікування.
    void Consume();

    bool active() const { return pipe_.valid(); }

private:
    bool CreateAndListen();

    Handle     pipe_;
    Handle     event_;
    OVERLAPPED overlapped_{};
    Handler    handler_;
    bool       pending_ = false;
};

/// Клієнтська частина: викликається в режимі --hook.
///
/// @returns true, якщо повідомлення доставлено робочому Bridge
bool SendHookMessage(const HookMessage& message, uint32_t timeoutMs = 2000);

/// Розбирає JSON, який Claude Code подає хуку на стандартний вхід.
bool ParseHookInput(std::string_view json, std::string_view eventName, HookMessage& out);

/// Читає весь стандартний вхід. Обмежений за розміром, щоб зіпсований
/// або зловмисний вхід не з'їв пам'ять.
std::string ReadStdinAll(size_t maxBytes = 256 * 1024);

}  // namespace cm
