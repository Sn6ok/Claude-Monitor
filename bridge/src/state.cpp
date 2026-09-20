// state.cpp — машина станів і керування набором сесій.

#include "state.h"
#include "transcript.h"

namespace cm {

// ── Текстові представлення ───────────────────────────────────────────────────
//
// Використовуються і в протоколі, і в журналі. Рядки стабільні: Android
// покладається на них при розборі, тому змінювати їх не можна без підняття
// версії протоколу.

const char* ToString(ClaudeState state) {
    switch (state) {
        case ClaudeState::Unknown:  return "unknown";
        case ClaudeState::Starting: return "starting";
        case ClaudeState::Idle:     return "idle";
        case ClaudeState::Working:  return "working";
        case ClaudeState::Waiting:  return "waiting";
        case ClaudeState::Error:    return "error";
        case ClaudeState::Limited:  return "limited";
        case ClaudeState::Finished: return "finished";
        case ClaudeState::Gone:     return "gone";
    }
    return "unknown";
}

const char* ToString(EventKind kind) {
    switch (kind) {
        case EventKind::Status:   return "status";
        case EventKind::Activity: return "activity";
        case EventKind::Output:   return "output";
        case EventKind::Result:   return "result";
        case EventKind::Session:  return "session";
        case EventKind::Prompt:   return "prompt";
        case EventKind::Node:     return "node";
    }
    return "output";
}

const char* ToString(ActivityAction action) {
    switch (action) {
        case ActivityAction::Other:      return "other";
        case ActivityAction::ReadFile:   return "read_file";
        case ActivityAction::EditFile:   return "edit_file";
        case ActivityAction::WriteFile:  return "write_file";
        case ActivityAction::RunCommand: return "run_command";
        case ActivityAction::Search:     return "search";
        case ActivityAction::WebFetch:   return "web_fetch";
        case ActivityAction::Task:       return "task";
        case ActivityAction::Subagent:   return "subagent";
    }
    return "other";
}

const char* ToString(TitleSource source) {
    switch (source) {
        case TitleSource::None:    return "none";
        case TitleSource::Custom:  return "custom";
        case TitleSource::Derived: return "derived";
        case TitleSource::Prompt:  return "prompt";
        case TitleSource::Path:    return "path";
    }
    return "none";
}

const char* ToString(BridgeState state) {
    switch (state) {
        case BridgeState::Starting:     return "starting";
        case BridgeState::Detecting:    return "detecting";
        case BridgeState::Unpaired:     return "unpaired";
        case BridgeState::Connecting:   return "connecting";
        case BridgeState::Monitoring:   return "monitoring";
        case BridgeState::Reconnecting: return "reconnecting";
        case BridgeState::ShuttingDown: return "shutting_down";
        case BridgeState::Stopped:      return "stopped";
    }
    return "stopped";
}

// ── SessionManager ───────────────────────────────────────────────────────────

namespace {

std::string LastSegment(std::string_view path) {
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.remove_suffix(1);
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

}  // namespace

SessionManager::SessionManager(EventSink sink) : sink_(std::move(sink)) {}

void SessionManager::SetNodeRules(std::vector<NodeCommandRule> rules) {
    nodeRules_ = std::move(rules);
}

std::string SessionManager::MatchNodeRule(const Event& event, std::string& nodeName) const {
    if (event.kind != EventKind::Activity) return {};
    if (event.action != ActivityAction::RunCommand) return {};
    if (nodeRules_.empty() || event.target.empty()) return {};

    // Перше правило, яке підійшло. Один рядок на команду: мод не має права
    // перетворити чат на потік власних повідомлень.
    for (const NodeCommandRule& rule : nodeRules_) {
        std::string text = ApplyNodeCommandRule(rule, event.target);
        if (text.empty()) continue;
        nodeName = rule.nodeName;
        return text;
    }
    return {};
}

void SessionManager::Emit(Event&& event) {
    // Команду міг упізнати вузол — тоді слідом за дією в чат іде його рядок
    // («Натискаю: Файл»). Задача та сама, що виконала команду: приписувати
    // рядок навмання нікуди не доводиться.
    std::string nodeName;
    const std::string nodeText = MatchNodeRule(event, nodeName);
    const std::string nodeSessionId = nodeText.empty() ? std::string() : event.sessionId;
    const uint64_t nodeTimestampMs = event.timestampMs;

    event.sequence = nextSequence();

    // Копія події осідає в буфері своєї сесії — саме з нього формується
    // snapshot після перепідключення телефона.
    const auto it = sessions_.find(event.sessionId);
    if (it != sessions_.end()) {
        it->second.lastSequence = event.sequence;
        it->second.lastUpdateMs = event.timestampMs;
        it->second.pushRecent(event);
    }

    if (sink_) sink_(std::move(event));

    if (!nodeText.empty()) {
        Event note;
        note.sessionId = nodeSessionId;
        note.timestampMs = nodeTimestampMs;
        note.kind = EventKind::Node;
        note.priority = Priority::Medium;
        note.target = nodeName;   // чий це рядок — видно в застосунку
        note.text = nodeText;
        Emit(std::move(note));
    }
}

SessionState& SessionManager::AddSession(const DiscoveredSession& discovered) {
    SessionState session;
    session.sessionId      = discovered.sessionId;
    session.pid            = discovered.pid;
    session.cwd            = discovered.cwd;
    session.startedAtMs    = discovered.startedAtMs;
    session.transcriptPath = discovered.transcriptPath;
    session.project        = LastSegment(discovered.cwd);
    session.state          = ClaudeState::Starting;
    session.lastUpdateMs   = NowUnixMs();

    // Назва від Claude Code краща за назву каталогу, але поступається
    // явно заданій назві чи тексту запиту — ті прийдуть із транскрипту.
    if (!discovered.name.empty()) {
        session.title = TruncateUtf8(SanitizeText(discovered.name), 120);
        session.titleSource = TitleSource::Derived;
    } else if (!session.project.empty()) {
        session.title = session.project;
        session.titleSource = TitleSource::Path;
    }

    auto [it, inserted] = sessions_.emplace(session.sessionId, std::move(session));

    Event event;
    event.sessionId   = it->second.sessionId;
    event.kind        = EventKind::Session;
    event.state       = ClaudeState::Starting;
    event.timestampMs = NowUnixMs();
    event.priority    = Priority::High;
    event.text        = "started";
    Emit(std::move(event));

    return it->second;
}

void SessionManager::RetireSession(const std::string& sessionId, ClaudeState finalState) {
    const auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;

    Event event;
    event.sessionId   = sessionId;
    event.kind        = EventKind::Session;
    event.state       = finalState;
    event.timestampMs = NowUnixMs();
    event.priority    = Priority::High;
    event.text        = "ended";
    Emit(std::move(event));

    SessionState retired = std::move(it->second);
    retired.state = finalState;
    retired.alive = false;
    sessions_.erase(it);

    // Історія завершених задач обмежена: користувач бачить, що задача
    // завершилась, але накопичувати їх без меж не можна.
    finished_.push_back(std::move(retired));
    while (finished_.size() > kMaxFinishedHistory) finished_.pop_front();
}

void SessionManager::Reconcile(const std::vector<DiscoveredSession>& discovered) {
    // Крок 1: нові та оновлені сесії.
    for (const DiscoveredSession& item : discovered) {
        if (item.sessionId.empty()) continue;

        auto it = sessions_.find(item.sessionId);
        if (it == sessions_.end()) {
            // Верхня межа кількості одночасних задач. Перевищення означає
            // нетипову ситуацію; замість необмеженого росту просто не беремо
            // нову сесію під нагляд (docs/architecture.md §5.1).
            if (sessions_.size() >= kMaxSessions) continue;

            SessionState& session = AddSession(item);

            // Сесія могла початися до запуску Bridge — читаємо хвіст
            // транскрипту, щоб одразу показати актуальний стан.
            //
            // Ці події НЕ йдуть у чергу відправлення: вони вже сталися,
            // подекуди години тому. Інакше телефон при кожному підключенні
            // отримував би лавину старої історії з давніми позначками часу
            // упереміш зі свіжими подіями. Вони лише наповнюють буфер
            // знімка, з якого будується початковий стан задачі.
            if (!session.transcriptPath.empty()) {
                const std::string sessionId = session.sessionId;
                TranscriptReader::ReadTail(session, 60, [this, &sessionId](Event&& event) {
                    const auto it = sessions_.find(sessionId);
                    if (it == sessions_.end()) return;

                    event.sequence = ++sequence_;
                    it->second.lastSequence = event.sequence;
                    it->second.lastUpdateMs = event.timestampMs;
                    it->second.pushRecent(event);
                });
            }
            continue;
        }

        // Шлях до транскрипту міг зʼявитися пізніше за саму сесію.
        if (it->second.transcriptPath.empty() && !item.transcriptPath.empty()) {
            it->second.transcriptPath = item.transcriptPath;
        }
        it->second.alive = true;
    }

    // Крок 2: сесії, яких більше немає в реєстрі.
    std::vector<std::string> vanished;
    for (auto& [sessionId, session] : sessions_) {
        bool stillThere = false;
        for (const DiscoveredSession& item : discovered) {
            if (item.sessionId == sessionId) { stillThere = true; break; }
        }
        if (!stillThere) vanished.push_back(sessionId);
    }

    for (const std::string& sessionId : vanished) {
        const auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) continue;

        // Розрізняємо коректне завершення й раптове зникнення процесу.
        // Стан Gone — це чесна відповідь «сесія зникла», а не вигадане
        // «успішно завершено» (Частина 6 §42 Master Prompt).
        const ClaudeState finalState =
            (it->second.state == ClaudeState::Idle || it->second.state == ClaudeState::Finished)
                ? ClaudeState::Finished
                : ClaudeState::Gone;

        RetireSession(sessionId, finalState);
    }
}

size_t SessionManager::PollTranscripts() {
    size_t produced = 0;

    for (auto& [sessionId, session] : sessions_) {
        if (session.transcriptPath.empty()) continue;

        TranscriptReader::ReadNew(session, [this, &produced](Event&& event) {
            ++produced;
            Emit(std::move(event));
        });
    }
    return produced;
}

void SessionManager::MarkWaiting(const std::string& sessionId, std::string_view reason) {
    const auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;

    if (it->second.state == ClaudeState::Waiting) return;  // без дублювання
    it->second.state = ClaudeState::Waiting;

    Event event;
    event.sessionId   = sessionId;
    event.kind        = EventKind::Status;
    event.state       = ClaudeState::Waiting;
    event.timestampMs = NowUnixMs();
    event.priority    = Priority::High;
    event.text        = TruncateUtf8(SanitizeText(reason), kMaxEventText);
    // Очікування дозволу — частина тієї самої відповіді: таймер іде далі.
    if (it->second.turnStartKnown) event.turnStartedAtMs = it->second.turnStartedAtMs;
    Emit(std::move(event));
}

void SessionManager::MarkFinished(const std::string& sessionId) {
    const auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    it->second.state = ClaudeState::Finished;
    RetireSession(sessionId, ClaudeState::Finished);
}

bool SessionManager::ReleaseExpiredLimits(uint64_t nowMs) {
    bool changed = false;
    for (auto& [sessionId, session] : sessions_) {
        if (session.state != ClaudeState::Limited || session.limitResetAtMs == 0) continue;
        if (nowMs < session.limitResetAtMs) continue;

        session.state = ClaudeState::Idle;
        session.limitResetAtMs = 0;
        changed = true;
    }
    return changed;
}

Summary SessionManager::BuildSummary() const {
    Summary summary;
    summary.active = static_cast<uint32_t>(sessions_.size());

    for (const auto& [sessionId, session] : sessions_) {
        switch (session.state) {
            case ClaudeState::Working:  summary.working += 1; break;
            case ClaudeState::Waiting:  summary.waiting += 1; break;
            case ClaudeState::Idle:     summary.idle += 1;    break;
            case ClaudeState::Error:    summary.error += 1;   break;
            default: break;
        }
    }

    for (const SessionState& session : finished_) {
        if (session.state == ClaudeState::Error) summary.error += 1;
        else summary.finished += 1;
    }

    return summary;
}

}  // namespace cm
