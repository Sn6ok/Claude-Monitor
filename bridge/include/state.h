// state.h — керування станом усіх відстежуваних сесій.
//
// Це єдине джерело істини Bridge про те, що відбувається. Стан кожної сесії
// зберігається окремо в асоціативному контейнері за ідентифікатором сесії:
// глобального поля «поточний стан» не існує, тому дві задачі принципово
// не можуть перезаписати стан одна одної (docs/architecture.md §5.1).

#pragma once

#include "events.h"
#include "sessions.h"

#include <functional>
#include <map>
#include <string>

namespace cm {

/// Стан самого Bridge (Частина 5 §40 Master Prompt).
enum class BridgeState : uint8_t {
    Starting = 0,
    Detecting,        ///< шукає процес Claude Desktop
    Unpaired,         ///< немає ключів, потрібен pairing
    Connecting,       ///< встановлює зв'язок із Relay
    Monitoring,       ///< основний робочий стан
    Reconnecting,
    ShuttingDown,
    Stopped,
};

const char* ToString(BridgeState state);

class SessionManager {
public:
    explicit SessionManager(EventSink sink);

    /// Звіряє внутрішній стан із результатом сканування реєстру:
    /// додає нові сесії, позначає зниклі, оновлює метадані.
    /// Викликається у відповідь на подію файлової системи, не за таймером.
    void Reconcile(const std::vector<DiscoveredSession>& discovered);

    /// Дочитує транскрипти всіх активних сесій.
    /// @returns кількість породжених подій
    size_t PollTranscripts();

    /// Знімає ліміти, час скидання яких уже минув: задача знову очікує на запит.
    /// Подія в стрічку не йде — позначка «ЛІМІТ» там лишається як була;
    /// змінюється лише стан задачі.
    /// @returns true, якщо стан хоч однієї задачі змінився
    bool ReleaseExpiredLimits(uint64_t nowMs);

    /// Обробляє сигнал від hook: сесія очікує на дію користувача.
    void MarkWaiting(const std::string& sessionId, std::string_view reason);

    /// Обробляє сигнал від hook: сесія завершилась.
    void MarkFinished(const std::string& sessionId);

    Summary BuildSummary() const;

    const std::map<std::string, SessionState>& sessions() const { return sessions_; }
    const std::deque<SessionState>& finished() const { return finished_; }

    uint64_t nextSequence() { return ++sequence_; }
    uint64_t currentSequence() const { return sequence_; }

private:
    SessionState& AddSession(const DiscoveredSession& discovered);
    void RetireSession(const std::string& sessionId, ClaudeState finalState);
    void Emit(Event&& event);

    EventSink sink_;
    std::map<std::string, SessionState> sessions_;

    /// Завершені задачі лишаються видимими деякий час, але їхня кількість
    /// жорстко обмежена: історія не має рости необмежено (Частина 4 §21).
    std::deque<SessionState> finished_;

    uint64_t sequence_ = 0;
};

}  // namespace cm
