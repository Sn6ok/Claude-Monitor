// test_transcript.cpp — тести нормалізації транскрипту.
//
// Формат рядків узятий із реальних транскриптів цієї машини
// (docs/research.md §3.2), а не вигаданий.

#include "test_framework.h"
#include "common.h"
#include "transcript.h"

#include <cstdio>
#include <vector>

using namespace cm;

namespace {

/// Прогоняє рядки через нормалізатор і збирає породжені події.
struct Harness {
    SessionState session;
    std::vector<Event> events;

    Harness() {
        session.sessionId = "test-session-0001";
        session.cwd = "D:\\Projects\\Demo";
    }

    /// Рядок цілком: кінець відповіді оголошується одразу, як після
    /// дочитаного файлу.
    void Feed(std::string_view line) {
        FeedHeld(line);
        FlushHeldTurnEnd(session, [this](Event&& event) { events.push_back(std::move(event)); });
    }

    /// Рядок посеред читання файлу: кінець відповіді ще може бути притримано.
    void FeedHeld(std::string_view line) {
        ProcessTranscriptLine(line, session, [this](Event&& event) {
            events.push_back(std::move(event));
        });
    }

    void FlushHeld() {
        FlushHeldTurnEnd(session, [this](Event&& event) { events.push_back(std::move(event)); });
    }

    const Event* FirstOfKind(EventKind kind) const {
        for (const Event& event : events) {
            if (event.kind == kind) return &event;
        }
        return nullptr;
    }

    size_t CountOfKind(EventKind kind) const {
        size_t count = 0;
        for (const Event& event : events) {
            if (event.kind == kind) ++count;
        }
        return count;
    }
};

}  // namespace

CM_TEST(transcript, maps_tool_names_to_actions) {
    CHECK(MapToolToAction("Read") == ActivityAction::ReadFile);
    CHECK(MapToolToAction("Edit") == ActivityAction::EditFile);
    CHECK(MapToolToAction("MultiEdit") == ActivityAction::EditFile);
    CHECK(MapToolToAction("Write") == ActivityAction::WriteFile);
    CHECK(MapToolToAction("Bash") == ActivityAction::RunCommand);
    CHECK(MapToolToAction("Grep") == ActivityAction::Search);
    CHECK(MapToolToAction("WebFetch") == ActivityAction::WebFetch);
    CHECK(MapToolToAction("Task") == ActivityAction::Task);
}

CM_TEST(transcript, unknown_tool_becomes_other_not_invented) {
    // Master Prompt (Частина 5 §12) забороняє вигадувати категорії.
    CHECK(MapToolToAction("mcp__tradingview__ui_evaluate") == ActivityAction::Other);
    CHECK(MapToolToAction("ЩосьНовеУМайбутнійВерсії") == ActivityAction::Other);
    CHECK(MapToolToAction("") == ActivityAction::Other);
}

CM_TEST(transcript, extracts_tool_use_activity) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-09T11:39:50.969Z",
        "message":{"content":[{"type":"tool_use","name":"Edit",
        "input":{"file_path":"src/main.cpp","old_string":"a","new_string":"b"}}]}})");

    const Event* activity = harness.FirstOfKind(EventKind::Activity);
    CHECK(activity != nullptr);
    if (activity) {
        CHECK(activity->action == ActivityAction::EditFile);
        CHECK_STR(activity->target, "src/main.cpp");
        // Вміст правок на телефон не передається — лише шлях.
        CHECK(activity->text.empty());
    }
    CHECK(harness.session.state == ClaudeState::Working);
    CHECK(harness.session.pendingToolUse);
}

CM_TEST(transcript, long_command_arrives_whole_in_parts) {
    Harness harness;
    std::string longCommand(8000, 'x');
    harness.Feed(std::string(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","name":"Bash","input":{"command":")") + longCommand + R"("}}]}})");

    // Команда не обрізається: довга ділиться на частини, які разом дають її всю.
    std::string joined;
    size_t parts = 0;
    for (const Event& event : harness.events) {
        if (event.kind != EventKind::Activity) continue;
        CHECK(event.target.size() <= kMaxEventText);
        joined += event.target;
        ++parts;
    }
    CHECK(parts > 1);
    CHECK(joined == longCommand);

    // Картка задачі в переліку бере лише початок.
    CHECK(harness.session.currentTarget.size() <= kMaxActivityPreview + 8);
}

CM_TEST(transcript, ordinary_command_is_shown_whole) {
    Harness harness;
    // Саме такої довжини звичайний виклик збірки — колишня межа
    // у 160 байтів різала його посеред шляху.
    const std::string command =
        "\\\"C:\\\\Program Files (x86)\\\\Microsoft Visual Studio\\\\2022\\\\BuildTools"
        "\\\\Common7\\\\Tools\\\\VsDevCmd.bat\\\" -arch=x64 && cd /d "
        "\\\"D:\\\\Wall\\\\Claude Monito\\\\bridge\\\\build\\\" && cmake --build . --config Release";
    harness.Feed(std::string(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","name":"Bash","input":{"command":")") + command + R"("}}]}})");

    const Event* activity = harness.FirstOfKind(EventKind::Activity);
    CHECK(activity != nullptr);
    if (activity) {
        CHECK(activity->target.find("cmake --build") != std::string::npos);
        // Багатокрапка означала б, що команду обрізано.
        CHECK(activity->target.find("\xE2\x80\xA6") == std::string::npos);
    }
}

CM_TEST(transcript, multiline_command_keeps_its_lines) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","name":"Bash","input":{"command":"cd build\nctest --output-on-failure"}}]}})");

    const Event* activity = harness.FirstOfKind(EventKind::Activity);
    CHECK(activity != nullptr);
    if (activity) CHECK_STR(activity->target, "cd build\nctest --output-on-failure");
}

CM_TEST(transcript, sensitive_file_path_is_hidden) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","name":"Read","input":{"file_path":"/app/.env"}}]}})");

    const Event* activity = harness.FirstOfKind(EventKind::Activity);
    CHECK(activity != nullptr);
    if (activity) CHECK(activity->target.find(".env") == std::string::npos);
}

CM_TEST(transcript, returning_to_work_announces_state_change) {
    Harness harness;

    // Спершу задача завершує крок і переходить в очікування.
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"Готово"}]}})");
    CHECK(harness.session.state == ClaudeState::Idle);

    const size_t statusesBefore = harness.CountOfKind(EventKind::Status);

    // Далі надходить новий виклик інструмента — робота відновилася.
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","name":"Bash","input":{"command":"ls"}}]}})");

    CHECK(harness.session.state == ClaudeState::Working);

    // Без окремої події про стан приймач бачив би лише активність,
    // а мітка задачі лишалася б «очікує».
    CHECK(harness.CountOfKind(EventKind::Status) > statusesBefore);

    bool announcedWorking = false;
    for (const Event& event : harness.events) {
        if (event.kind == EventKind::Status && event.state == ClaudeState::Working) {
            announcedWorking = true;
        }
    }
    CHECK(announcedWorking);
}

CM_TEST(transcript, end_turn_produces_idle_status) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"Готово"}]}})");

    const Event* status = harness.FirstOfKind(EventKind::Status);
    CHECK(status != nullptr);
    if (status) CHECK(status->state == ClaudeState::Idle);
    CHECK(harness.session.state == ClaudeState::Idle);
}

CM_TEST(transcript, tool_result_marked_as_error_is_error) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"content":[
        {"type":"tool_result","is_error":true}]},
        "toolUseResult":{"stdout":"","stderr":"error: ляпнуло","interrupted":false}})");

    const Event* result = harness.FirstOfKind(EventKind::Result);
    CHECK(result != nullptr);
    if (result) {
        CHECK(result->isError);
        CHECK(result->priority == Priority::High);  // помилки не витісняються
    }
    CHECK(harness.session.state == ClaudeState::Error);
}

CM_TEST(transcript, stderr_without_error_flag_is_not_error) {
    Harness harness;

    // Оболонка й багато програм пишуть у stderr звичайні повідомлення.
    // Позначати це помилкою означало б показувати червоне там, де все гаразд.
    harness.Feed(R"({"type":"user","message":{"content":[{"type":"tool_result"}]},
        "toolUseResult":{"stdout":"","stderr":"Shell cwd was reset to D:\\Projects",
        "interrupted":false}})");

    const Event* result = harness.FirstOfKind(EventKind::Result);
    CHECK(result != nullptr);
    if (result) CHECK_FALSE(result->isError);
    CHECK(harness.session.state != ClaudeState::Error);
}

CM_TEST(transcript, git_progress_in_stderr_is_not_error) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"content":[{"type":"tool_result"}]},
        "toolUseResult":{"stdout":"","stderr":"Switched to branch 'main'","interrupted":false}})");

    const Event* result = harness.FirstOfKind(EventKind::Result);
    if (result) CHECK_FALSE(result->isError);
}

CM_TEST(transcript, empty_successful_result_produces_no_event) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"content":[{"type":"tool_result"}]},
        "toolUseResult":{"stdout":"","stderr":"","interrupted":false}})");

    // Порожній успіх не несе інформації і лише засмічував би стрічку.
    CHECK_EQ(harness.CountOfKind(EventKind::Result), 0u);
    CHECK_FALSE(harness.session.pendingToolUse);
}

CM_TEST(transcript, custom_title_wins_over_prompt) {
    Harness harness;
    harness.Feed(R"({"type":"last-prompt","lastPrompt":"зроби щось"})");
    CHECK(harness.session.titleSource == TitleSource::Prompt);

    harness.Feed(R"({"type":"custom-title","customTitle":"Рефакторинг мережі"})");
    CHECK(harness.session.titleSource == TitleSource::Custom);
    CHECK_STR(harness.session.title, "Рефакторинг мережі");

    // Наступний запит не має перебивати явно задану назву.
    harness.Feed(R"({"type":"last-prompt","lastPrompt":"продовжуй"})");
    CHECK_STR(harness.session.title, "Рефакторинг мережі");
}

CM_TEST(transcript, first_prompt_becomes_title_not_last) {
    Harness harness;
    harness.Feed(R"({"type":"last-prompt","lastPrompt":"Виправити авторизацію"})");
    harness.Feed(R"({"type":"last-prompt","lastPrompt":"продовжуй"})");

    // Останній запит часто є коротким «продовжуй» і суті задачі не описує.
    CHECK_STR(harness.session.title, "Виправити авторизацію");
}

CM_TEST(transcript, detached_head_branch_is_ignored) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","gitBranch":"HEAD","message":{"content":[]}})");
    // "HEAD" — технічне значення, користувачу воно нічого не каже.
    CHECK(harness.session.gitBranch.empty());
}

CM_TEST(transcript, real_branch_is_kept) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","gitBranch":"feature/auth","message":{"content":[]}})");
    CHECK_STR(harness.session.gitBranch, "feature/auth");
}

CM_TEST(transcript, project_name_derived_from_cwd) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"content":[]}})");
    CHECK_STR(harness.session.project, "Demo");
}

CM_TEST(transcript, malformed_lines_do_not_break_reading) {
    Harness harness;

    harness.Feed("це не json");
    harness.Feed("{незакритий");
    harness.Feed("");
    harness.Feed("null");
    harness.Feed("[]");

    // Після сміття нормальний рядок має оброблятися як звичайно:
    // Claude Code міг саме зараз дописувати файл.
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"tool_use","name":"Read","input":{"file_path":"a.txt"}}]}})");

    CHECK_EQ(harness.CountOfKind(EventKind::Activity), 1u);
}

CM_TEST(transcript, oversized_line_is_skipped) {
    Harness harness;
    const std::string huge(2 * 1024 * 1024, 'x');
    harness.Feed(huge);
    CHECK_EQ(harness.events.size(), 0u);
}

CM_TEST(transcript, every_event_carries_session_id) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[
        {"type":"tool_use","name":"Bash","input":{"command":"ls"}},
        {"type":"text","text":"готово"}]}})");

    CHECK(harness.events.size() > 0);
    for (const Event& event : harness.events) {
        // Це технічна гарантія ізоляції задач: подія без sid не існує.
        CHECK_STR(event.sessionId, "test-session-0001");
    }
}

CM_TEST(transcript, secrets_in_output_are_masked) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"text","text":"встановив PASSWORD=hunter2 у конфіг"}]}})");

    const Event* output = harness.FirstOfKind(EventKind::Output);
    CHECK(output != nullptr);
    if (output) CHECK(output->text.find("hunter2") == std::string::npos);
}

CM_TEST(transcript, event_text_respects_size_limit) {
    Harness harness;
    const std::string longText(5000, 'a');
    harness.Feed(std::string(R"({"type":"assistant","message":{"content":[
        {"type":"text","text":")") + longText + R"("}]}})");

    const Event* output = harness.FirstOfKind(EventKind::Output);
    CHECK(output != nullptr);
    if (output) CHECK(output->text.size() <= kMaxEventText + 8);
}

CM_TEST(transcript, line_breaks_survive_to_the_event) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"text","text":"Перший рядок\nДругий рядок\n1. пункт"}]}})");

    const Event* output = harness.FirstOfKind(EventKind::Output);
    CHECK(output != nullptr);
    if (output) {
        // Без переносів текст злипається в одну стрічку, і ані списки,
        // ані вивід команд у ній уже не читаються.
        CHECK(output->text.find('\n') != std::string::npos);
        CHECK_STR(output->text, "Перший рядок\nДругий рядок\n1. пункт");
    }
}

// ── Підготовка тексту ────────────────────────────────────────────────────────

CM_TEST(text, multiline_keeps_line_structure) {
    CHECK_STR(SanitizeMultiline("рядок 1\nрядок 2\nрядок 3"),
              "рядок 1\nрядок 2\nрядок 3");
}

CM_TEST(text, multiline_keeps_column_alignment) {
    // У виводі команд пробілами вирівняні колонки: стиснувши їх,
    // ми зруйнували б саме те, заради чого цей вивід і дивляться.
    CHECK_STR(SanitizeMultiline("  OK    перший\n  OK    другий"),
              "  OK    перший\n  OK    другий");
}

CM_TEST(text, multiline_drops_trailing_spaces_and_extra_blank_lines) {
    CHECK_STR(SanitizeMultiline("\n\nабзац   \n\n\n\nдругий\n\n"),
              "абзац\n\nдругий");
}

CM_TEST(text, multiline_expands_tabs) {
    // Ширина табуляції на телефоні непередбачувана.
    CHECK_STR(SanitizeMultiline("a\tb"), "a    b");
}

CM_TEST(text, multiline_removes_carriage_returns_and_control_chars) {
    CHECK_STR(SanitizeMultiline("рядок\r\nдалі\x07."), "рядок\nдалі.");
}

CM_TEST(text, single_line_sanitizer_still_flattens) {
    // Назви задач і шляхи мусять лишатися однорядковими.
    CHECK_STR(SanitizeText("назва\nз переносом"), "назва з переносом");
    CHECK_STR(SanitizeText("зайві    пробіли"), "зайві пробіли");
}

// ── Довгі репліки та запити користувача ──────────────────────────────────────

CM_TEST(transcript, long_reply_is_split_into_parts_not_cut) {
    Harness harness;
    std::string longText;
    for (int i = 0; i < 400; ++i) {
        longText += "рядок відповіді номер " + std::to_string(i) + "\\n";
    }
    harness.Feed(std::string(R"({"type":"assistant","message":{"content":[
        {"type":"text","text":")") + longText + R"("}]}})");

    size_t parts = 0;
    uint64_t group = 0;
    std::string joined;
    for (const Event& event : harness.events) {
        if (event.kind != EventKind::Output) continue;

        CHECK(event.text.size() <= kMaxEventText);
        CHECK(event.part == parts);
        // Розрив не посеред символу UTF-8: інакше на екрані були б кракозябри.
        CHECK((static_cast<unsigned char>(event.text[0]) & 0xC0) != 0x80);

        if (parts == 0) group = event.groupId;
        CHECK(event.groupId == group);
        ++parts;
        joined += event.text;
    }

    CHECK(parts > 1);
    CHECK(group != 0);
    // Склеєні частини дають увесь текст — від першого рядка до останнього.
    CHECK(joined.find("номер 0\n") != std::string::npos);
    CHECK(joined.find("номер 399") != std::string::npos);
    CHECK(joined.find("\xE2\x80\xA6") == std::string::npos);
}

CM_TEST(transcript, short_reply_is_one_event_without_parts) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"content":[
        {"type":"text","text":"коротко"}]}})");

    const Event* output = harness.FirstOfKind(EventKind::Output);
    CHECK(output != nullptr);
    if (output) {
        CHECK(output->partCount == 1);
        CHECK(output->groupId == 0);
    }
}

CM_TEST(transcript, user_prompt_becomes_prompt_event) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"role":"user",
        "content":"виправ це і надішли додаток"}})");

    const Event* prompt = harness.FirstOfKind(EventKind::Prompt);
    CHECK(prompt != nullptr);
    if (prompt) CHECK_STR(prompt->text, "виправ це і надішли додаток");
}

CM_TEST(transcript, prompt_with_images_carries_count_not_bytes) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"role":"user","content":[
        {"type":"image","source":{"type":"base64","data":"AAAABBBB"}},
        {"type":"image","source":{"type":"base64","data":"CCCCDDDD"}},
        {"type":"text","text":"подивись на фото"}]}})");

    const Event* prompt = harness.FirstOfKind(EventKind::Prompt);
    CHECK(prompt != nullptr);
    if (prompt) {
        // Текст — без службових позначок: кількість зображень іде окремим полем,
        // щоб застосунок написав її своєю мовою.
        CHECK_STR(prompt->text, "подивись на фото");
        CHECK(prompt->imageCount == 2u);
        CHECK(prompt->text.find("AAAABBBB") == std::string::npos);
    }
}

CM_TEST(transcript, prompt_without_text_but_with_image_is_shown) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"role":"user","content":[
        {"type":"image","source":{"type":"base64","data":"AAAABBBB"}}]}})");

    const Event* prompt = harness.FirstOfKind(EventKind::Prompt);
    CHECK(prompt != nullptr);
    if (prompt) {
        CHECK(prompt->text.empty());
        CHECK(prompt->imageCount == 1u);
    }
    CHECK(harness.session.state == ClaudeState::Working);
}

CM_TEST(transcript, service_user_records_are_not_prompts) {
    Harness harness;
    harness.Feed(R"({"type":"user","isMeta":true,"message":{"content":"службове"}})");
    harness.Feed(R"({"type":"user","isCompactSummary":true,
        "message":{"content":"This session is being continued"}})");
    harness.Feed(R"({"type":"user","message":{"content":"<command-name>/clear</command-name>"}})");
    harness.Feed(R"({"type":"user","isSidechain":true,"message":{"content":"завдання субагенту"}})");

    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 0u);
}

CM_TEST(transcript, tool_result_is_not_a_prompt) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"content":[{"type":"tool_result"}]},
        "toolUseResult":{"stdout":"готово","stderr":"","interrupted":false}})");

    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 0u);
}

// ── Швидкість зміни стану ────────────────────────────────────────────────────

namespace {

size_t CountStatus(const Harness& harness, ClaudeState state) {
    size_t count = 0;
    for (const Event& event : harness.events) {
        if (event.kind == EventKind::Status && event.state == state) ++count;
    }
    return count;
}

}  // namespace

CM_TEST(transcript, prompt_switches_to_working_immediately) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"Готово"}]}})");
    CHECK(harness.session.state == ClaudeState::Idle);

    // Одного запиту досить: чекати на виклик інструмента не треба.
    harness.Feed(R"({"type":"user","message":{"role":"user","content":"продовжуй"}})");

    CHECK(harness.session.state == ClaudeState::Working);
    CHECK_EQ(CountStatus(harness, ClaudeState::Working), 1u);
}

CM_TEST(transcript, thinking_mid_turn_announces_working) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"Готово"}]}})");

    // Саме так у транскрипті починається хід: спершу думки зі stop_reason
    // tool_use. Раніше стан мінявся тут мовчки, без події.
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"tool_use",
        "content":[{"type":"thinking","thinking":"..."}]}})");

    CHECK(harness.session.state == ClaudeState::Working);
    CHECK_EQ(CountStatus(harness, ClaudeState::Working), 1u);
}

CM_TEST(transcript, text_reply_mid_turn_announces_working) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"Готово"}]}})");
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"tool_use",
        "content":[{"type":"text","text":"Зараз перевірю"}]}})");

    CHECK(harness.session.state == ClaudeState::Working);
    CHECK_EQ(CountStatus(harness, ClaudeState::Working), 1u);
}

CM_TEST(transcript, working_is_announced_once_not_per_line) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"role":"user","content":"зроби"}})");
    for (int i = 0; i < 10; ++i) {
        harness.Feed(R"({"type":"assistant","message":{"stop_reason":"tool_use",
            "content":[{"type":"tool_use","name":"Read","input":{"file_path":"a.txt"}}]}})");
        harness.Feed(R"({"type":"user","message":{"content":[{"type":"tool_result"}]},
            "toolUseResult":{"stdout":"ok","stderr":"","interrupted":false}})");
    }

    // Десять кроків роботи — одна подія про стан, а не двадцять.
    CHECK_EQ(CountStatus(harness, ClaudeState::Working), 1u);
}

CM_TEST(transcript, interrupt_returns_to_idle_without_prompt_bubble) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"role":"user","content":"зроби"}})");
    CHECK(harness.session.state == ClaudeState::Working);

    harness.Feed(R"({"type":"user","message":{"role":"user",
        "content":[{"type":"text","text":"[Request interrupted by user]"}]}})");

    CHECK(harness.session.state == ClaudeState::Idle);
    CHECK_EQ(CountStatus(harness, ClaudeState::Idle), 1u);
    // Службовий рядок не видається за повідомлення користувача.
    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 1u);
}

CM_TEST(transcript, service_records_do_not_start_work) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"Готово"}]}})");
    harness.Feed(R"({"type":"user","isMeta":true,"message":{"content":"службове"}})");
    harness.Feed(R"({"type":"user","message":{"content":"<command-name>/clear</command-name>"}})");

    CHECK(harness.session.state == ClaudeState::Idle);
}

// ── Час відповіді ────────────────────────────────────────────────────────────

namespace {

const Event* LastStatus(const Harness& harness, ClaudeState state) {
    const Event* found = nullptr;
    for (const Event& event : harness.events) {
        if (event.kind == EventKind::Status && event.state == state) found = &event;
    }
    return found;
}

}  // namespace

CM_TEST(transcript, turn_duration_is_measured_from_prompt_to_end) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    CHECK(harness.session.turnStartedAtMs != 0);
    CHECK(harness.session.turnStartKnown);

    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:01:30.000Z",
        "message":{"stop_reason":"tool_use","content":[{"type":"thinking","thinking":"..."}]}})");
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:03:12.500Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Готово"}]}})");

    CHECK(harness.session.state == ClaudeState::Idle);
    CHECK(harness.session.lastTurnMs == 192500u);   // 3 хв 12,5 с
    CHECK(harness.session.turnStartedAtMs == 0u);

    // Тривалість їде на телефон у тій самій події, що повідомляє про кінець.
    const Event* idle = LastStatus(harness, ClaudeState::Idle);
    CHECK(idle != nullptr);
    if (idle) CHECK(idle->turnMs == 192500u);
}

CM_TEST(transcript, working_status_carries_turn_start) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");

    // Без початку відповіді в самій події телефон не зміг би запустити таймер.
    const Event* working = LastStatus(harness, ClaudeState::Working);
    CHECK(working != nullptr);
    if (working) CHECK(working->turnStartedAtMs == harness.session.turnStartedAtMs);
}

CM_TEST(transcript, tool_error_does_not_restart_turn_timer) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    const uint64_t started = harness.session.turnStartedAtMs;

    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:30.000Z",
        "message":{"content":[{"type":"tool_result","is_error":true}]},
        "toolUseResult":{"stdout":"","stderr":"впало","interrupted":false}})");
    CHECK(harness.session.state == ClaudeState::Error);

    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:00:40.000Z",
        "message":{"stop_reason":"tool_use","content":[
        {"type":"tool_use","name":"Bash","input":{"command":"ls"}}]}})");

    CHECK(harness.session.state == ClaudeState::Working);
    CHECK(harness.session.turnStartedAtMs == started);
}

CM_TEST(transcript, queued_prompt_mid_turn_keeps_timer) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    const uint64_t started = harness.session.turnStartedAtMs;

    // Повідомлення, надіслане, поки Claude ще відповідає, — не нова відповідь.
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:20.000Z",
        "message":{"role":"user","content":"і ще перевір тести"}})");

    CHECK(harness.session.turnStartedAtMs == started);
}

CM_TEST(transcript, interrupted_turn_reports_its_duration) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:45.000Z",
        "message":{"role":"user","content":[{"type":"text","text":"[Request interrupted by user]"}]}})");

    CHECK(harness.session.state == ClaudeState::Idle);
    CHECK(harness.session.lastTurnMs == 45000u);
}

CM_TEST(transcript, turn_seen_from_the_middle_has_no_duration) {
    // Bridge запустився посеред відповіді й початку не бачив: вигадувати
    // тривалість із першого побаченого рядка не можна.
    Harness harness;
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:01:00.000Z",
        "message":{"stop_reason":"tool_use","content":[{"type":"thinking","thinking":"..."}]}})");

    const Event* working = LastStatus(harness, ClaudeState::Working);
    CHECK(working != nullptr);
    if (working) CHECK(working->turnStartedAtMs == 0u);

    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:02:00.000Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Готово"}]}})");

    CHECK(harness.session.state == ClaudeState::Idle);
    CHECK(harness.session.lastTurnMs == 0u);
}

CM_TEST(transcript, turn_closed_after_silence_still_reports_duration) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зібрати проєкт"}})");

    // Стан уже «очікує», а відповідь іще відкрита. Коли вона справді
    // завершиться, тривалість має бути повною, а не загубитися.
    harness.session.state = ClaudeState::Idle;

    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:07:00.000Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Зібрано"}]}})");

    CHECK(harness.session.lastTurnMs == 420000u);
    const Event* idle = LastStatus(harness, ClaudeState::Idle);
    CHECK(idle != nullptr);
    if (idle) CHECK(idle->turnMs == 420000u);
}

// ── Повний вивід команд ──────────────────────────────────────────────────────

CM_TEST(transcript, long_command_output_arrives_whole) {
    Harness harness;
    std::string stdoutText;
    for (int i = 0; i < 300; ++i) {
        stdoutText += "рядок виводу " + std::to_string(i) + "\\n";
    }
    harness.Feed(std::string(R"({"type":"user","message":{"content":[{"type":"tool_result"}]},
        "toolUseResult":{"stdout":")") + stdoutText +
        R"(","stderr":"попередження наприкінці","interrupted":false}})");

    std::string joined;
    for (const Event& event : harness.events) {
        if (event.kind == EventKind::Result) joined += event.text;
    }

    CHECK(joined.find("рядок виводу 0\n") != std::string::npos);
    CHECK(joined.find("рядок виводу 299") != std::string::npos);
    // stderr не витісняє stdout і сам не губиться.
    CHECK(joined.find("попередження наприкінці") != std::string::npos);
    CHECK(joined.find("\xE2\x80\xA6") == std::string::npos);
}

// ── Повідомлення, надіслані посеред роботи ───────────────────────────────────

CM_TEST(transcript, prompt_sent_mid_turn_is_shown_once) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    const uint64_t started = harness.session.turnStartedAtMs;
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:00:05.000Z",
        "message":{"stop_reason":"tool_use","content":[{"type":"thinking","thinking":"..."}]}})");

    // Ви надсилаєте ще одне повідомлення, поки Claude працює.
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue",
        "timestamp":"2026-09-13T10:00:30.000Z","content":"і ще перевір тести"})");
    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 2u);

    // Claude отримує його посеред відповіді — повтору в стрічці немає.
    harness.Feed(R"({"type":"attachment","timestamp":"2026-09-13T10:00:30.000Z",
        "attachment":{"type":"queued_command","prompt":"і ще перевір тести",
        "commandMode":"prompt","origin":{"kind":"human"}}})");
    harness.Feed(R"({"type":"queue-operation","operation":"remove",
        "timestamp":"2026-09-13T10:00:40.000Z","content":"і ще перевір тести"})");

    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 2u);
    CHECK(harness.session.state == ClaudeState::Working);
    CHECK(harness.session.turnStartedAtMs == started);
}

CM_TEST(transcript, prompt_through_queue_is_not_duplicated) {
    Harness harness;
    // Навіть звичайний запит проходить через чергу: enqueue, dequeue, рядок user.
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue",
        "timestamp":"2026-09-13T10:00:00.000Z","content":"привіт"})");
    harness.Feed(R"({"type":"queue-operation","operation":"dequeue",
        "timestamp":"2026-09-13T10:00:00.010Z"})");
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.020Z",
        "message":{"role":"user","content":"привіт"}})");

    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 1u);
    CHECK(harness.session.state == ClaudeState::Working);
}

CM_TEST(transcript, queued_message_keeps_turn_going) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue",
        "timestamp":"2026-09-13T10:00:30.000Z","content":"і ще"})");

    // Claude закінчив відповідь раніше, ніж дійшов до вашого повідомлення.
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:01:00.000Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Готово"}]}})");
    CHECK(harness.session.state == ClaudeState::Working);
    CHECK_EQ(harness.CountOfKind(EventKind::Status) > 0 &&
             harness.session.lastTurnMs == 0, true);

    // Воно стає початком наступної відповіді — для вас це та сама розмова.
    harness.Feed(R"({"type":"queue-operation","operation":"dequeue",
        "timestamp":"2026-09-13T10:01:00.100Z"})");
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:01:00.200Z",
        "message":{"role":"user","content":"і ще"}})");
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:02:00.000Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Теж готово"}]}})");

    CHECK(harness.session.state == ClaudeState::Idle);
    CHECK(harness.session.lastTurnMs == 120000u);   // від першого запиту до кінця
    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 2u);
}

CM_TEST(transcript, cancelled_queued_message_ends_turn) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue",
        "timestamp":"2026-09-13T10:00:30.000Z","content":"і ще"})");
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T10:01:00.000Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Готово"}]}})");
    CHECK(harness.session.state == ClaudeState::Working);

    // Повідомлення прибрали з черги — нової відповіді не буде.
    harness.Feed(R"({"type":"queue-operation","operation":"remove",
        "timestamp":"2026-09-13T10:01:05.000Z","content":"і ще"})");
    CHECK(harness.session.state == ClaudeState::Idle);
}

CM_TEST(transcript, same_text_in_two_turns_is_shown_twice) {
    Harness harness;
    harness.Feed(R"({"type":"user","message":{"role":"user","content":"продовжуй"}})");
    harness.Feed(R"({"type":"assistant","message":{"stop_reason":"end_turn",
        "content":[{"type":"text","text":"ок"}]}})");
    harness.Feed(R"({"type":"user","message":{"role":"user","content":"продовжуй"}})");

    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 2u);
}

CM_TEST(transcript, queued_image_is_shown_on_delivery) {
    Harness harness;
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue"})");
    harness.Feed(R"({"type":"attachment","attachment":{"type":"queued_command",
        "prompt":[{"type":"image","source":{"type":"base64","data":"AAAA"}},
        {"type":"text","text":"подивись"}],"commandMode":"prompt","origin":{"kind":"human"}}})");

    const Event* prompt = harness.FirstOfKind(EventKind::Prompt);
    CHECK(prompt != nullptr);
    if (prompt) {
        CHECK(prompt->imageCount == 1u);
        CHECK(prompt->text.find("подивись") != std::string::npos);
    }
}

CM_TEST(transcript, queued_slash_command_is_not_a_message) {
    Harness harness;
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue","content":"/compact"})");
    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 0u);
}

// ── Ліміт і тривалість обірваних відповідей ──────────────────────────────────

CM_TEST(transcript, usage_limit_is_its_own_state) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T19:04:48.000Z",
        "message":{"role":"user","content":"продовж роботу"}})");
    harness.Feed(R"json({"type":"assistant","timestamp":"2026-09-13T19:06:13.000Z",
        "isApiErrorMessage":true,"error":"rate_limit","message":{"model":"<synthetic>",
        "stop_reason":"stop_sequence","content":[{"type":"text",
        "text":"You've hit your session limit · resets 10:30pm (Europe/Kyiv)"}]}})json");

    CHECK(harness.session.state == ClaudeState::Limited);
    CHECK_STR(ToString(ClaudeState::Limited), "limited");

    const Event* limited = LastStatus(harness, ClaudeState::Limited);
    CHECK(limited != nullptr);
    if (limited) {
        CHECK_STR(limited->text, "10:30pm (Europe/Kyiv)");
        CHECK(limited->turnMs == 85000u);
    }
    // Саме повідомлення про ліміт теж лишається в стрічці.
    CHECK_EQ(harness.CountOfKind(EventKind::Output), 1u);
}

CM_TEST(transcript, prompt_after_limit_starts_new_turn) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T19:04:48.000Z",
        "message":{"role":"user","content":"продовж роботу"}})");
    harness.Feed(R"json({"type":"assistant","timestamp":"2026-09-13T19:06:13.000Z",
        "isApiErrorMessage":true,"error":"rate_limit","message":{"stop_reason":"stop_sequence",
        "content":[{"type":"text","text":"You've hit your session limit · resets 10:30pm (Europe/Kyiv)"}]}})json");

    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T19:31:00.000Z",
        "message":{"role":"user","content":"продовж"}})");
    CHECK(harness.session.state == ClaudeState::Working);

    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T19:32:00.000Z",
        "message":{"stop_reason":"end_turn","content":[{"type":"text","text":"Готово"}]}})");
    CHECK(harness.session.lastTurnMs == 60000u);
}

CM_TEST(transcript, limit_does_not_wait_for_queued_message) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T10:00:00.000Z",
        "message":{"role":"user","content":"зроби"}})");
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue",
        "timestamp":"2026-09-13T10:00:30.000Z","content":"і ще"})");
    harness.Feed(R"json({"type":"assistant","timestamp":"2026-09-13T10:01:00.000Z",
        "isApiErrorMessage":true,"error":"rate_limit","message":{"stop_reason":"stop_sequence",
        "content":[{"type":"text","text":"You've hit your session limit · resets 5pm (Europe/Kyiv)"}]}})json");

    // Поки ліміт не скинуто, Claude за повідомлення з черги не візьметься.
    CHECK(harness.session.state == ClaudeState::Limited);
}

CM_TEST(transcript, prompt_after_long_silence_starts_fresh_turn) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-09T10:30:32.000Z",
        "message":{"role":"user","content":"прочитай master prompt"}})");
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-09T10:31:00.000Z",
        "message":{"stop_reason":"tool_use","content":[
        {"type":"tool_use","name":"Read","input":{"file_path":"a.txt"}}]}})");

    // Кінця відповіді в транскрипті немає: Claude Code закрили посеред роботи.
    // Через чотири дні — новий запит і переривання за 3 хв 18 с.
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T17:45:23.852Z",
        "message":{"role":"user","content":"продовж роботу"}})");
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T17:48:41.961Z",
        "message":{"role":"user","content":[{"type":"text","text":"[Request interrupted by user]"}]}})");

    CHECK(harness.session.lastTurnMs == 198109u);
}

CM_TEST(transcript, head_of_file_does_not_leak_turn_into_tail) {
    // Саме цей випадок дав «відповідь за 103 год»: на початку файлу — перший
    // запит сесії, далі великий запис, у хвості — робота й переривання.
    // Голову Bridge читає лише заради теки й назви, хвіст — заради стану.
    wchar_t directory[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, directory);
    const std::wstring path = std::wstring(directory) + L"cm_head_leak_test.jsonl";

    std::string content;
    content += R"({"type":"user","timestamp":"2026-09-09T10:30:32.000Z","cwd":"D:\\Projects\\Demo",)"
               R"("message":{"role":"user","content":"прочитай master prompt"}})" "\n";
    content += R"({"type":"assistant","timestamp":"2026-09-09T10:31:00.000Z","message":{)"
               R"("stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read",)"
               R"("input":{"file_path":"a.txt"}}]}})" "\n";
    // Запис, більший за вікно голови: голова закінчується посеред відповіді.
    content += R"({"type":"attachment","timestamp":"2026-09-10T08:00:00.000Z",)"
               R"("attachment":{"type":"file","content":")" + std::string(150 * 1024, 'x') + "\"}}\n";
    content += R"({"type":"assistant","timestamp":"2026-09-13T17:47:59.000Z","message":{)"
               R"("stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read",)"
               R"("input":{"file_path":"b.txt"}}]}})" "\n";
    content += R"({"type":"user","timestamp":"2026-09-13T17:48:41.961Z","message":{"role":"user",)"
               R"("content":[{"type":"text","text":"[Request interrupted by user]"}]}})" "\n";

    FILE* file = _wfopen(path.c_str(), L"wb");
    CHECK(file != nullptr);
    if (!file) return;
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);

    SessionState session;
    session.sessionId = "tail-session";
    session.transcriptPath = path;

    std::vector<Event> events;
    TranscriptReader::ReadTail(session, 2, [&events](Event&& event) {
        events.push_back(std::move(event));
    });
    ::DeleteFileW(path.c_str());

    // Початку цієї відповіді Bridge не бачив — тривалості бути не повинно.
    CHECK(session.lastTurnMs == 0u);
    for (const Event& event : events) CHECK(event.turnMs == 0u);
    CHECK(session.state == ClaudeState::Idle);

    // А тека з голови файлу при цьому підхопилась.
    CHECK_STR(session.cwd, "D:\\Projects\\Demo");
}

// ── Час скидання ліміту ──────────────────────────────────────────────────────

namespace {

SYSTEMTIME LocalTimeOf(uint64_t unixMs) {
    ULARGE_INTEGER value{};
    value.QuadPart = unixMs * 10000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    ::FileTimeToSystemTime(&ft, &utc);
    ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}

}  // namespace

CM_TEST(transcript, limit_reset_is_next_local_moment) {
    const uint64_t message = NowUnixMs();
    const uint64_t reset = ParseLimitResetMs("10:30pm (Europe/Kyiv)", message);

    CHECK(reset > message);
    // Найближчий такий момент — не далі ніж за добу (із запасом на перехід часу).
    CHECK(reset - message <= 25ULL * 3600ULL * 1000ULL);

    const SYSTEMTIME local = LocalTimeOf(reset);
    CHECK(local.wHour == 22);
    CHECK(local.wMinute == 30);
}

CM_TEST(transcript, limit_reset_with_date) {
    const uint64_t message = NowUnixMs();
    const uint64_t reset = ParseLimitResetMs("Aug 23, 5pm (Europe/Kyiv)", message);

    CHECK(reset > message);
    const SYSTEMTIME local = LocalTimeOf(reset);
    CHECK(local.wMonth == 8);
    CHECK(local.wDay == 23);
    CHECK(local.wHour == 17);
    CHECK(local.wMinute == 0);
}

CM_TEST(transcript, limit_reset_unknown_format_is_zero) {
    CHECK(ParseLimitResetMs("soon", NowUnixMs()) == 0u);
    CHECK(ParseLimitResetMs("25pm", NowUnixMs()) == 0u);
    CHECK(ParseLimitResetMs("10:75pm", NowUnixMs()) == 0u);
    CHECK(ParseLimitResetMs("", NowUnixMs()) == 0u);
    CHECK(ParseLimitResetMs("10:30pm", 0) == 0u);
}

CM_TEST(transcript, usage_limit_remembers_reset_moment) {
    Harness harness;
    harness.Feed(R"json({"type":"assistant","timestamp":"2026-09-13T19:06:13.000Z",
        "isApiErrorMessage":true,"error":"rate_limit","message":{"stop_reason":"stop_sequence",
        "content":[{"type":"text","text":"You've hit your session limit · resets 10:30pm (Europe/Kyiv)"}]}})json");

    CHECK(harness.session.state == ClaudeState::Limited);
    CHECK(harness.session.limitResetAtMs > 0u);

    // Новий стан скасовує запам'ятований момент: ліміт уже не діє.
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T19:31:00.000Z",
        "message":{"role":"user","content":"продовж"}})");
    CHECK(harness.session.limitResetAtMs == 0u);
}

// ── Текст у блоках thinking ─────────────────────────────────────────────────

CM_TEST(transcript, thinking_with_text_is_shown_as_reply) {
    // Саме так Claude Code записав повідомлення, яке не дійшло до телефона.
    Harness harness;
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T20:52:50.664Z","message":{)"
                 R"("stop_reason":"tool_use","content":[{"type":"thinking",)"
                 R"("thinking":"Далі створюю головну Activity.\n\n","signature":"abc"}]}})");

    const Event* output = harness.FirstOfKind(EventKind::Output);
    CHECK(output != nullptr);
    if (output) CHECK_STR(output->text, "Далі створюю головну Activity.");
    CHECK(harness.session.state == ClaudeState::Working);
}

CM_TEST(transcript, empty_thinking_is_not_a_reply) {
    Harness harness;
    harness.Feed(R"({"type":"assistant","timestamp":"2026-09-13T20:52:50.664Z","message":{)"
                 R"("stop_reason":"tool_use","content":[{"type":"thinking",)"
                 R"("thinking":"","signature":"abc"}]}})");

    CHECK_EQ(harness.CountOfKind(EventKind::Output), 0u);
    CHECK(harness.session.state == ClaudeState::Working);
}

// ── Початок відповіді після перезапуску Bridge ──────────────────────────────

namespace {

std::string IsoTimestamp(uint64_t unixMs) {
    ULARGE_INTEGER value{};
    value.QuadPart = unixMs * 10000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    SYSTEMTIME utc{};
    ::FileTimeToSystemTime(&ft, &utc);
    char text[40]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
                  utc.wMilliseconds);
    return text;
}

std::string PromptLine(uint64_t ms, const std::string& text) {
    return R"({"type":"user","timestamp":")" + IsoTimestamp(ms) +
           R"(","message":{"role":"user","content":")" + text + "\"}}\n";
}

std::string EndTurnLine(uint64_t ms) {
    return R"({"type":"assistant","timestamp":")" + IsoTimestamp(ms) +
           R"(","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}})" "\n";
}

/// Виклик інструмента із заповненням — щоб рядки не влізли у вікно хвоста.
std::string ToolUseLine(uint64_t ms, size_t padding) {
    return R"({"type":"assistant","timestamp":")" + IsoTimestamp(ms) +
           R"(","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read",)"
           R"("input":{"file_path":")" + std::string(padding, 'p') + "\"}}]}}\n";
}

/// Записує транскрипт у тимчасовий файл і читає його хвіст, як після запуску Bridge.
void ReadTailOf(const std::string& content, SessionState& session) {
    wchar_t directory[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, directory);
    const std::wstring path = std::wstring(directory) + L"cm_turn_start_test.jsonl";

    FILE* file = _wfopen(path.c_str(), L"wb");
    CHECK(file != nullptr);
    if (!file) return;
    std::fwrite(content.data(), 1, content.size(), file);
    std::fclose(file);

    session.sessionId = "tail-turn";
    session.transcriptPath = path;
    TranscriptReader::ReadTail(session, 3, [](Event&&) {});
    ::DeleteFileW(path.c_str());
}

constexpr uint64_t kMinute = 60ULL * 1000ULL;

}  // namespace

CM_TEST(transcript, tail_finds_start_of_running_turn) {
    // Bridge перезапустили посеред довгої відповіді: запит, з якого вона
    // почалась, лежить далеко за вікном хвоста. Посеред роботи прийшло ще
    // одне повідомлення — відлік воно не скидає.
    const uint64_t now = NowUnixMs();
    const uint64_t promptAt = now - 25 * kMinute;

    std::string content;
    content += PromptLine(promptAt - 3 * kMinute, "перший запит");
    content += EndTurnLine(promptAt - 2 * kMinute);
    content += PromptLine(promptAt, "так є вже виправлення");
    for (int i = 0; i < 4; ++i) content += ToolUseLine(promptAt + (i + 1) * kMinute, 60 * 1024);
    content += PromptLine(promptAt + 6 * kMinute, "і ще одне посеред роботи");
    for (int i = 0; i < 4; ++i) content += ToolUseLine(promptAt + (i + 7) * kMinute, 60 * 1024);
    content += ToolUseLine(now - 5000, 16);

    SessionState session;
    ReadTailOf(content, session);

    CHECK(session.state == ClaudeState::Working);
    CHECK(session.turnStartKnown);
    CHECK(session.turnStartedAtMs == promptAt);
}

CM_TEST(transcript, tail_does_not_borrow_prompt_from_finished_turn) {
    // Перед хвостом відповідь уже завершилась, а нового запиту немає —
    // початок невідомий, і вигадувати його не можна.
    const uint64_t now = NowUnixMs();

    std::string content;
    content += PromptLine(now - 40 * kMinute, "давній запит");
    content += EndTurnLine(now - 39 * kMinute);
    for (int i = 0; i < 6; ++i) content += ToolUseLine(now - (20 - i) * kMinute, 60 * 1024);
    content += ToolUseLine(now - 5000, 16);

    SessionState session;
    ReadTailOf(content, session);

    CHECK(session.state == ClaudeState::Working);
    CHECK_FALSE(session.turnStartKnown);
}

CM_TEST(transcript, tail_of_long_silent_turn_keeps_start_unknown) {
    // Понад пів години без жодного запису — відповідь обірвалась.
    const uint64_t now = NowUnixMs();
    const uint64_t old = now - 3 * 60 * kMinute;

    std::string content;
    content += PromptLine(old, "давній запит");
    for (int i = 0; i < 6; ++i) content += ToolUseLine(old + (i + 1) * kMinute, 60 * 1024);

    SessionState session;
    ReadTailOf(content, session);

    CHECK_FALSE(session.turnStartKnown);
}

// ── Позначка «ГОТОВО» після останнього блоку репліки ────────────────────────

namespace {

/// Номер першої події заданого виду (і стану — для подій про стан) або -1.
int IndexOf(const std::vector<Event>& events, EventKind kind,
            ClaudeState state = ClaudeState::Unknown) {
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind != kind) continue;
        if (kind == EventKind::Status && events[i].state != state) continue;
        return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

CM_TEST(transcript, done_marker_follows_last_block_of_reply) {
    // Саме так Claude Code записав відповідь, після якої «ГОТОВО» стало не там:
    // спершу блок thinking уже з ознакою кінця ходу, за ним окремим рядком — текст.
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T19:58:00.000Z",
        "message":{"role":"user","content":"виправ"}})");
    harness.FeedHeld(R"({"type":"assistant","timestamp":"2026-09-13T21:18:31.000Z","message":{)"
                     R"("id":"msg_final","stop_reason":"end_turn",)"
                     R"("content":[{"type":"thinking","thinking":"","signature":"s"}]}})");

    // Стан змінюється одразу, а позначка чекає решти репліки.
    CHECK(harness.session.state == ClaudeState::Idle);
    CHECK(IndexOf(harness.events, EventKind::Status, ClaudeState::Idle) == -1);

    harness.FeedHeld(R"({"type":"assistant","timestamp":"2026-09-13T21:18:48.000Z","message":{)"
                     R"("id":"msg_final","stop_reason":"end_turn",)"
                     R"("content":[{"type":"text","text":"Надіслав новий APK."}]}})");
    harness.FlushHeld();

    const int output = IndexOf(harness.events, EventKind::Output);
    const int done = IndexOf(harness.events, EventKind::Status, ClaudeState::Idle);
    CHECK(output >= 0);
    CHECK(done > output);

    // Час і тривалість — за останнім блоком: 19:58:00 → 21:18:48.
    if (done >= 0 && output >= 0) {
        CHECK(harness.events[done].turnMs == 4848000u);
        CHECK(harness.events[done].timestampMs == harness.events[output].timestampMs);
    }
    CHECK(harness.session.lastTurnMs == 4848000u);
    CHECK_EQ(harness.CountOfKind(EventKind::Status), 2u);  // працює, готово
}

CM_TEST(transcript, held_done_marker_is_released_by_next_record) {
    Harness harness;
    harness.Feed(R"({"type":"user","timestamp":"2026-09-13T20:00:00.000Z",
        "message":{"role":"user","content":"привіт"}})");
    harness.FeedHeld(R"({"type":"assistant","timestamp":"2026-09-13T20:00:05.000Z","message":{)"
                     R"("id":"msg_a","stop_reason":"end_turn","content":[{"type":"text","text":"Вітаю"}]}})");
    CHECK(IndexOf(harness.events, EventKind::Status, ClaudeState::Idle) == -1);

    // Будь-який інший запис означає, що репліку дописано.
    harness.FeedHeld(R"({"type":"last-prompt","lastPrompt":"привіт","sessionId":"x"})");
    const int done = IndexOf(harness.events, EventKind::Status, ClaudeState::Idle);
    CHECK(done > IndexOf(harness.events, EventKind::Output));
    if (done >= 0) CHECK(harness.events[done].turnMs == 5000u);
}

// ── Службові записи Claude Code ─────────────────────────────────────────────

CM_TEST(transcript, task_notification_from_queue_is_not_your_message) {
    // Так Claude Code повідомляє себе про завершення фонової команди. У стрічці
    // це виглядало як «ваше повідомлення», хоча користувач його не писав.
    Harness harness;
    harness.Feed(R"json({"type":"queue-operation","operation":"enqueue",
        "content":"<task-notification>\n<task-id>bcj658pnr</task-id>\n<status>completed</status>\n</task-notification>"})json");

    CHECK_EQ(harness.CountOfKind(EventKind::Prompt), 0u);
}

CM_TEST(transcript, queued_message_from_you_is_still_shown) {
    // Перевірка, що фільтр службових записів не з'їв звичайне повідомлення.
    Harness harness;
    harness.Feed(R"({"type":"queue-operation","operation":"enqueue","content":"продовж роботу"})");

    const Event* prompt = harness.FirstOfKind(EventKind::Prompt);
    CHECK(prompt != nullptr);
    if (prompt) CHECK_STR(prompt->text, "продовж роботу");
}
