// test_nodes.cpp — вузли: маніфест, вивід і межі.
//
// Запуск справжніх процесів тут не перевіряється — перевіряється все, що
// вирішує, ЩО саме побачить користувач.

#include "test_framework.h"
#include "nodes.h"
#include "state.h"
#include "wire.h"

#include <string>
#include <vector>

using namespace cm;

namespace {

NodeManifest Manifest(const char* json, const char* folder = "system-info") {
    return ParseNodeManifest(json, folder);
}

}  // namespace

CM_TEST(nodes, manifest_reads_fields) {
    const NodeManifest node = Manifest(
        R"({"name":"Система","run":"run.ps1","intervalSec":15,"timeoutSec":5,"version":"1.0.0"})");

    CHECK(node.valid);
    CHECK_STR(node.id, "system-info");
    CHECK_STR(node.name, "Система");
    CHECK_STR(node.run, "run.ps1");
    CHECK(node.intervalSec == 15u);
    CHECK(node.timeoutSec == 5u);
    CHECK(node.enabled);
}

CM_TEST(nodes, manifest_without_run_is_not_a_node) {
    const NodeManifest node = Manifest(R"({"name":"Без запуску"})");
    CHECK_FALSE(node.valid);
    CHECK(!node.problem.empty());
}

CM_TEST(nodes, broken_manifest_is_reported_not_guessed) {
    const NodeManifest node = Manifest("це не json");
    CHECK_FALSE(node.valid);
    CHECK(!node.problem.empty());
}

CM_TEST(nodes, manifest_limits_are_enforced) {
    // Вузол не має права ні молотити щосекунди, ні висіти пів години.
    const NodeManifest node = Manifest(R"({"run":"run.ps1","intervalSec":1,"timeoutSec":600})");
    CHECK(node.valid);
    CHECK(node.intervalSec == kMinNodeIntervalSec);
    CHECK(node.timeoutSec == kMaxNodeTimeoutSec);
}

CM_TEST(nodes, disabled_node_is_marked) {
    const NodeManifest node = Manifest(R"({"run":"run.ps1","enabled":false})");
    CHECK(node.valid);
    CHECK_FALSE(node.enabled);
}

CM_TEST(nodes, output_becomes_card) {
    const NodeManifest node = Manifest(R"({"name":"Система","run":"run.ps1"})");
    const NodeResult card = ParseNodeOutput(node,
        R"({"status":"warn","lines":[{"label":"CPU","value":"12%"},)"
        R"({"label":"Памʼять","value":"7.4 ГБ"}],"text":"усе гаразд"})", 1000);

    CHECK_STR(card.id, "system-info");
    CHECK_STR(card.name, "Система");
    CHECK_STR(card.status, "warn");
    CHECK_EQ(card.lines.size(), 2u);
    if (card.lines.size() == 2) {
        CHECK_STR(card.lines[0].label, "CPU");
        CHECK_STR(card.lines[1].value, "7.4 ГБ");
    }
    CHECK_STR(card.text, "усе гаразд");
    CHECK(card.error == NodeError::None);
    CHECK(card.updatedAtMs == 1000u);
}

CM_TEST(nodes, unknown_status_becomes_ok) {
    const NodeManifest node = Manifest(R"({"run":"run.ps1"})");
    const NodeResult card =
        ParseNodeOutput(node, R"({"status":"катастрофа","lines":[{"label":"a","value":"b"}]})", 1);
    CHECK_STR(card.status, "ok");
}

CM_TEST(nodes, not_json_is_an_honest_error) {
    const NodeManifest node = Manifest(R"({"run":"run.ps1"})");
    const NodeResult card = ParseNodeOutput(node, "просто текст", 1);

    CHECK_STR(card.status, "error");
    CHECK(card.error == NodeError::BadOutput);
    CHECK(card.lines.empty());
}

CM_TEST(nodes, empty_card_is_not_data) {
    // Вузол, який нічого не сказав, не має показувати порожню картку.
    const NodeManifest node = Manifest(R"({"run":"run.ps1"})");
    const NodeResult card = ParseNodeOutput(node, R"({"status":"ok","lines":[]})", 1);
    CHECK(card.error == NodeError::BadOutput);
}

CM_TEST(nodes, too_many_lines_are_cut) {
    const NodeManifest node = Manifest(R"({"run":"run.ps1"})");

    std::string json = R"({"lines":[)";
    for (int i = 0; i < 40; ++i) {
        if (i > 0) json += ",";
        json += R"({"label":"рядок","value":"значення"})";
    }
    json += "]}";

    const NodeResult card = ParseNodeOutput(node, json, 1);
    CHECK_EQ(card.lines.size(), kMaxNodeLines);
}

CM_TEST(nodes, long_values_are_trimmed) {
    const NodeManifest node = Manifest(R"({"run":"run.ps1"})");
    const std::string json =
        R"({"lines":[{"label":"x","value":")" + std::string(4000, 'v') + R"("}]})";

    const NodeResult card = ParseNodeOutput(node, json, 1);
    CHECK_EQ(card.lines.size(), 1u);
    if (!card.lines.empty()) CHECK(card.lines[0].value.size() <= kMaxNodeValue + 8);
}

CM_TEST(nodes, secrets_in_output_are_masked) {
    // Вузол може ненавмисно надрукувати токен — маскування те саме, що й
    // для звичайних подій.
    const NodeManifest node = Manifest(R"({"run":"run.ps1"})");
    const std::string json = std::string(R"({"lines":[{"label":"key","value":")") +
                             std::string("ghp") + "_abcdefghijklmnopqrstuvwxyz0123456789" +
                             R"("}]})";

    const NodeResult card = ParseNodeOutput(node, json, 1);
    CHECK_EQ(card.lines.size(), 1u);
    if (!card.lines.empty()) {
        CHECK(card.lines[0].value.find("abcdefghijklmnop") == std::string::npos);
    }
}

CM_TEST(nodes, command_line_picks_interpreter) {
    CHECK(BuildNodeCommandLine("run.ps1").find(L"powershell") != std::wstring::npos);
    CHECK(BuildNodeCommandLine("run.js").find(L"node ") != std::wstring::npos);
    CHECK(BuildNodeCommandLine("run.py").find(L"python ") != std::wstring::npos);
    CHECK(BuildNodeCommandLine("tool.exe") == L"tool.exe");

    // Готова команда з аргументами лишається як є.
    CHECK(BuildNodeCommandLine("node server.js --once") == L"node server.js --once");
    CHECK(BuildNodeCommandLine("").empty());
}

CM_TEST(nodes, failure_card_says_why) {
    const NodeManifest node = Manifest(R"({"name":"Система","run":"run.ps1"})");
    const NodeResult card = MakeNodeFailure(node, NodeError::Timeout, 5);

    CHECK_STR(card.status, "error");
    CHECK(card.error == NodeError::Timeout);
    CHECK_STR(card.name, "Система");
    CHECK_STR(std::string(ToString(NodeError::Timeout)), "timeout");
    CHECK_STR(std::string(ToString(NodeError::None)), "");
}

// ── Знімок стану ─────────────────────────────────────────────────────────────

CM_TEST(nodes, snapshot_carries_cards) {
    SessionManager manager([](Event&&) {});

    NodeResult ok;
    ok.id = "system-info";
    ok.name = "Система";
    ok.status = "ok";
    ok.lines.push_back({"Процесор", "32 %"});
    ok.text = "усе гаразд";
    ok.updatedAtMs = 1788957810000ULL;

    NodeResult failed = MakeNodeFailure(Manifest(R"({"name":"GPU","run":"run.ps1"})", "gpu"),
                                        NodeError::Timeout, 5);

    const std::vector<NodeResult> cards{ok, failed};
    const std::string json =
        BuildSnapshotJson(manager, "PC", 10, true, 0.1, 4.2, kMaxSnapshotEvents, &cards);

    CHECK(json.find("\"nodes\":[") != std::string::npos);
    CHECK(json.find("\"id\":\"system-info\"") != std::string::npos);
    CHECK(json.find("\"label\":\"Процесор\"") != std::string::npos);
    CHECK(json.find("\"value\":\"32 %\"") != std::string::npos);
    CHECK(json.find("\"updated_at\":1788957810000") != std::string::npos);

    // Причина відмови — кодом: текст напише застосунок своєю мовою.
    CHECK(json.find("\"error\":\"timeout\"") != std::string::npos);
}

CM_TEST(nodes, snapshot_without_nodes_has_no_field) {
    SessionManager manager([](Event&&) {});

    const std::string none =
        BuildSnapshotJson(manager, "PC", 10, true, 0.1, 4.2, kMaxSnapshotEvents, nullptr);
    CHECK(none.find("\"nodes\"") == std::string::npos);

    // Порожній перелік так само не займає місця в кадрі.
    const std::vector<NodeResult> empty;
    const std::string emptyJson =
        BuildSnapshotJson(manager, "PC", 10, true, 0.1, 4.2, kMaxSnapshotEvents, &empty);
    CHECK(emptyJson.find("\"nodes\"") == std::string::npos);
}

// ── Правила для чату ─────────────────────────────────────────────────────────

namespace {

const char* kClickManifest = R"({
    "id": "claude-click",
    "name": "Claude Click",
    "commands": [
        { "match": "click.py", "args": ["x", "y", "label"],
          "text": "Натискаю: {label}", "fallback": "Натискаю: точка {x}, {y}" }
    ]
})";

NodeCommandRule ClickRule() {
    return ParseNodeManifest(kClickManifest, "claude-click").commands.at(0);
}

}  // namespace

CM_TEST(nodes, manifest_reads_command_rules) {
    const NodeManifest node = ParseNodeManifest(kClickManifest, "claude-click");

    CHECK(node.valid);
    CHECK_EQ(node.commands.size(), 1u);
    CHECK_STR(node.commands[0].match, "click.py");
    CHECK_STR(node.commands[0].nodeName, "Claude Click");
    CHECK_EQ(node.commands[0].args.size(), 3u);
    CHECK_STR(node.commands[0].args[2], "label");

    // Вузол може лише підписувати команди — запускати в ньому нічого.
    CHECK(node.run.empty());
}

CM_TEST(nodes, manifest_without_run_and_commands_is_rejected) {
    const NodeManifest empty = Manifest(R"({"name":"Порожній"})");
    CHECK(!empty.valid);
    CHECK(!empty.problem.empty());

    // Правило без match або без тексту нічого не означає — його не беремо.
    const NodeManifest broken = Manifest(
        R"({"run":"run.ps1","commands":[{"text":"без match"},{"match":"x.py"}]})");
    CHECK(broken.valid);
    CHECK(broken.commands.empty());
}

CM_TEST(nodes, manifest_limits_number_of_rules) {
    std::string json = R"({"run":"run.ps1","commands":[)";
    for (size_t i = 0; i < kMaxNodeRules + 6; ++i) {
        if (i != 0) json += ",";
        json += R"({"match":"a.py","text":"t"})";
    }
    json += "]}";

    CHECK_EQ(Manifest(json.c_str()).commands.size(), kMaxNodeRules);
}

CM_TEST(nodes, rule_names_the_click) {
    const NodeCommandRule rule = ClickRule();

    // Рядок із лапками й шляхом — саме такий, яким його бачить Bridge.
    const std::string quoted = R"(python "D:\Claude Click\click.py" 640 300 "Файл")";
    const std::string full = ApplyNodeCommandRule(rule, quoted);
    CHECK_STR(full, "Натискаю: Файл");

    // Підпис із пробілами лишається цілим — лапки на те й потрібні.
    const std::string spacedCommand = R"(python click.py 10 20 "Зберегти як")";
    const std::string spaced = ApplyNodeCommandRule(rule, spacedCommand);
    CHECK_STR(spaced, "Натискаю: Зберегти як");

    // Регістр у шляхах Windows довільний.
    const std::string upperCommand = R"(python C:\tools\CLICK.PY 1 2 Меню)";
    const std::string upper = ApplyNodeCommandRule(rule, upperCommand);
    CHECK_STR(upper, "Натискаю: Меню");
}

CM_TEST(nodes, rule_falls_back_when_label_is_missing) {
    const NodeCommandRule rule = ClickRule();

    const std::string fallback = ApplyNodeCommandRule(rule, "python click.py 640 300");
    CHECK_STR(fallback, "Натискаю: точка 640, 300");

    // Немає ні підпису, ні координат — вигадувати нічого не будемо.
    CHECK(ApplyNodeCommandRule(rule, "python click.py").empty());
}

CM_TEST(nodes, rule_stops_at_shell_separators) {
    const NodeCommandRule rule = ClickRule();

    // Команда в транскрипті часто не одна: підпис не має тягти за собою «;».
    const std::string chained = ApplyNodeCommandRule(
        rule, "python click.py 10 20 \"Файл\"; echo done");
    CHECK_STR(chained, "Натискаю: Файл");

    const std::string andThen = ApplyNodeCommandRule(
        rule, "python click.py 10 20 Меню && python screenshot.py");
    CHECK_STR(andThen, "Натискаю: Меню");
}

CM_TEST(nodes, rule_ignores_other_commands) {
    const NodeCommandRule rule = ClickRule();

    CHECK(ApplyNodeCommandRule(rule, "npm test").empty());
    CHECK(ApplyNodeCommandRule(rule, "python screenshot.py").empty());
    CHECK(ApplyNodeCommandRule(rule, "").empty());
}

CM_TEST(nodes, rule_without_arguments_still_works) {
    const NodeManifest node = Manifest(
        R"({"run":"run.ps1","commands":[{"match":"screenshot.py","text":"Дивлюся на екран"}]})");

    const std::string shot = ApplyNodeCommandRule(node.commands.at(0), "python screenshot.py");
    CHECK_STR(shot, "Дивлюся на екран");
}

CM_TEST(nodes, rule_message_is_trimmed) {
    const NodeManifest node = Manifest(
        R"({"run":"run.ps1","commands":[{"match":"click.py","args":["label"],
            "text":"Натискаю: {label}"}]})");

    const std::string longLabel(400, 'x');
    const std::string message =
        ApplyNodeCommandRule(node.commands.at(0), "python click.py " + longLabel);

    CHECK(!message.empty());
    // Межа плюс позначка обрізання: «…» дописується вже після межі.
    CHECK(message.size() <= kMaxNodeMessage + 8);
}
