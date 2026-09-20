// test_state.cpp — тести мультизадачного менеджера сесій.
//
// Головне, що тут перевіряється: задачі ізольовані одна від одної.
// Це не побажання, а технічна гарантія (docs/architecture.md §5.1).

#include "test_framework.h"
#include "state.h"

#include <vector>

using namespace cm;

namespace {

DiscoveredSession MakeDiscovered(std::string_view sessionId,
                                 uint32_t pid,
                                 std::string_view cwd,
                                 std::string_view name = "") {
    DiscoveredSession session;
    session.sessionId = sessionId;
    session.pid = pid;
    session.cwd = cwd;
    session.name = name;
    session.startedAtMs = NowUnixMs();
    return session;
}

struct Harness {
    std::vector<Event> events;
    SessionManager manager;

    Harness() : manager([this](Event&& event) { events.push_back(std::move(event)); }) {}

    size_t EventsFor(std::string_view sessionId) const {
        size_t count = 0;
        for (const Event& event : events) {
            if (event.sessionId == sessionId) ++count;
        }
        return count;
    }
};

}  // namespace

CM_TEST(state, adds_discovered_sessions) {
    Harness harness;

    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\Projects\\Alpha", "alpha-1"),
        MakeDiscovered("sid-b", 200, "D:\\Projects\\Beta", "beta-2"),
    });

    CHECK_EQ(harness.manager.sessions().size(), 2u);

    const auto& sessions = harness.manager.sessions();
    CHECK(sessions.count("sid-a") == 1);
    CHECK(sessions.count("sid-b") == 1);
    CHECK_STR(sessions.at("sid-a").project, "Alpha");
    CHECK_STR(sessions.at("sid-b").project, "Beta");
}

CM_TEST(state, uses_registry_name_as_title) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\Demo", "demo-42")});

    const SessionState& session = harness.manager.sessions().at("sid-a");
    CHECK_STR(session.title, "demo-42");
    CHECK(session.titleSource == TitleSource::Derived);
}

CM_TEST(state, falls_back_to_directory_name) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\Projects\\Gamma")});

    const SessionState& session = harness.manager.sessions().at("sid-a");
    CHECK_STR(session.title, "Gamma");
    CHECK(session.titleSource == TitleSource::Path);
}

CM_TEST(state, sequence_is_monotonic_across_sessions) {
    Harness harness;
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-b", 200, "D:\\B"),
    });

    // Глобальна нумерація дає єдиний порядок для стрічки всіх задач.
    uint64_t previous = 0;
    for (const Event& event : harness.events) {
        CHECK(event.sequence > previous);
        previous = event.sequence;
    }
}

CM_TEST(state, every_emitted_event_has_session_id) {
    Harness harness;
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-b", 200, "D:\\B"),
    });
    harness.manager.MarkWaiting("sid-a", "потрібен дозвіл");

    CHECK(harness.events.size() > 0);
    for (const Event& event : harness.events) {
        CHECK(!event.sessionId.empty());
    }
}

CM_TEST(state, waiting_affects_only_target_session) {
    Harness harness;
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-b", 200, "D:\\B"),
    });

    harness.manager.MarkWaiting("sid-a", "дозвольте запис");

    // Стан однієї задачі не має протікати в іншу.
    CHECK(harness.manager.sessions().at("sid-a").state == ClaudeState::Waiting);
    CHECK(harness.manager.sessions().at("sid-b").state != ClaudeState::Waiting);
}

CM_TEST(state, waiting_is_not_duplicated) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\A")});

    const size_t before = harness.events.size();
    harness.manager.MarkWaiting("sid-a", "причина");
    harness.manager.MarkWaiting("sid-a", "причина");
    harness.manager.MarkWaiting("sid-a", "причина");

    // Повторний той самий стан не має породжувати нових подій:
    // це марний трафік (Частина 2 §6 Master Prompt).
    CHECK_EQ(harness.events.size(), before + 1);
}

CM_TEST(state, vanished_session_becomes_gone_not_finished) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\A")});

    // Сесія працювала і раптом зникла — це не «успішно завершено».
    harness.manager.Reconcile({});

    CHECK_EQ(harness.manager.sessions().size(), 0u);
    CHECK_EQ(harness.manager.finished().size(), 1u);
    CHECK(harness.manager.finished().back().state == ClaudeState::Gone);
}

CM_TEST(state, idle_session_that_vanishes_is_finished) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\A")});

    // Імітуємо коректне завершення роботи.
    const_cast<SessionState&>(harness.manager.sessions().at("sid-a")).state = ClaudeState::Idle;
    harness.manager.Reconcile({});

    CHECK(harness.manager.finished().back().state == ClaudeState::Finished);
}

CM_TEST(state, one_session_ending_leaves_others_running) {
    Harness harness;
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-b", 200, "D:\\B"),
        MakeDiscovered("sid-c", 300, "D:\\C"),
    });

    // Зникає лише одна задача.
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-c", 300, "D:\\C"),
    });

    CHECK_EQ(harness.manager.sessions().size(), 2u);
    CHECK(harness.manager.sessions().count("sid-a") == 1);
    CHECK(harness.manager.sessions().count("sid-c") == 1);
    CHECK(harness.manager.sessions().count("sid-b") == 0);
}

CM_TEST(state, session_limit_is_enforced) {
    Harness harness;

    std::vector<DiscoveredSession> many;
    for (uint32_t i = 0; i < kMaxSessions + 10; ++i) {
        many.push_back(MakeDiscovered("sid-" + std::to_string(i), 100 + i, "D:\\X"));
    }
    harness.manager.Reconcile(many);

    // Понад межу задачі під нагляд не беруться: необмежене зростання
    // пам'яті неприпустиме (Частина 4 §21 Master Prompt).
    CHECK(harness.manager.sessions().size() <= kMaxSessions);
}

CM_TEST(state, finished_history_is_bounded) {
    Harness harness;

    for (uint32_t i = 0; i < kMaxFinishedHistory + 15; ++i) {
        const std::string sessionId = "sid-" + std::to_string(i);
        harness.manager.Reconcile({MakeDiscovered(sessionId, 100 + i, "D:\\X")});
        harness.manager.Reconcile({});
    }

    CHECK(harness.manager.finished().size() <= kMaxFinishedHistory);
}

CM_TEST(state, summary_counts_states_separately) {
    Harness harness;
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-b", 200, "D:\\B"),
        MakeDiscovered("sid-c", 300, "D:\\C"),
    });

    auto& sessions = const_cast<std::map<std::string, SessionState>&>(harness.manager.sessions());
    sessions.at("sid-a").state = ClaudeState::Working;
    sessions.at("sid-b").state = ClaudeState::Working;
    sessions.at("sid-c").state = ClaudeState::Waiting;

    const Summary summary = harness.manager.BuildSummary();
    CHECK_EQ(summary.active, 3u);
    CHECK_EQ(summary.working, 2u);
    CHECK_EQ(summary.waiting, 1u);
    CHECK_EQ(summary.idle, 0u);
}

CM_TEST(state, error_in_one_session_does_not_mark_others) {
    Harness harness;
    harness.manager.Reconcile({
        MakeDiscovered("sid-a", 100, "D:\\A"),
        MakeDiscovered("sid-b", 200, "D:\\B"),
    });

    auto& sessions = const_cast<std::map<std::string, SessionState>&>(harness.manager.sessions());
    sessions.at("sid-a").state = ClaudeState::Error;
    sessions.at("sid-b").state = ClaudeState::Working;

    const Summary summary = harness.manager.BuildSummary();
    CHECK_EQ(summary.error, 1u);
    CHECK_EQ(summary.working, 1u);
}

CM_TEST(state, recent_events_buffer_is_bounded) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\A")});

    for (int i = 0; i < 100; ++i) {
        harness.manager.MarkWaiting("sid-a", "причина");
        // Скидаємо стан, щоб наступний виклик знову породив подію.
        const_cast<SessionState&>(harness.manager.sessions().at("sid-a")).state =
            ClaudeState::Working;
    }

    const SessionState& session = harness.manager.sessions().at("sid-a");
    CHECK(session.recentEvents.size() <= kMaxSnapshotEvents);
}

CM_TEST(state, unknown_session_ignored_safely) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\A")});

    const size_t before = harness.events.size();

    // Сигнал для неіснуючої задачі не має ані падати, ані приписуватися
    // наявній задачі.
    harness.manager.MarkWaiting("невідома-сесія", "причина");
    harness.manager.MarkFinished("невідома-сесія");

    CHECK_EQ(harness.events.size(), before);
    CHECK(harness.manager.sessions().at("sid-a").state != ClaudeState::Waiting);
}

CM_TEST(state, state_strings_are_stable) {
    // Ці рядки входять до протоколу: застосунок розбирає саме їх.
    CHECK_STR(ToString(ClaudeState::Working), "working");
    CHECK_STR(ToString(ClaudeState::Waiting), "waiting");
    CHECK_STR(ToString(ClaudeState::Idle), "idle");
    CHECK_STR(ToString(ClaudeState::Error), "error");
    CHECK_STR(ToString(ClaudeState::Unknown), "unknown");
    CHECK_STR(ToString(ClaudeState::Gone), "gone");

    CHECK_STR(ToString(ActivityAction::EditFile), "edit_file");
    CHECK_STR(ToString(ActivityAction::RunCommand), "run_command");
    CHECK_STR(ToString(ActivityAction::Other), "other");

    CHECK_STR(ToString(TitleSource::Custom), "custom");
    CHECK_STR(ToString(TitleSource::Prompt), "prompt");
}

// ── Зняття ліміту ────────────────────────────────────────────────────────────

CM_TEST(state, expired_limit_returns_task_to_idle_without_event) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\Projects\\Alpha")});

    auto& session = const_cast<SessionState&>(harness.manager.sessions().at("sid-a"));
    session.state = ClaudeState::Limited;
    session.limitResetAtMs = 5000;
    const size_t eventsBefore = harness.events.size();

    // До моменту скидання ліміт діє.
    CHECK_FALSE(harness.manager.ReleaseExpiredLimits(4999));
    CHECK(session.state == ClaudeState::Limited);

    // Щойно час настав — задача знову очікує, а стрічка лишається як була.
    CHECK(harness.manager.ReleaseExpiredLimits(5000));
    CHECK(session.state == ClaudeState::Idle);
    CHECK(session.limitResetAtMs == 0u);
    CHECK_EQ(harness.events.size(), eventsBefore);
}

CM_TEST(state, limit_without_known_reset_time_stays) {
    Harness harness;
    harness.manager.Reconcile({MakeDiscovered("sid-a", 100, "D:\\Projects\\Alpha")});

    auto& session = const_cast<SessionState&>(harness.manager.sessions().at("sid-a"));
    session.state = ClaudeState::Limited;
    session.limitResetAtMs = 0;

    // Час скидання невідомий — вигадувати його не можна, ліміт знімає лише запит.
    CHECK_FALSE(harness.manager.ReleaseExpiredLimits(NowUnixMs()));
    CHECK(session.state == ClaudeState::Limited);
}
