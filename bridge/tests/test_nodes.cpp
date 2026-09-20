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
