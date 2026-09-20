// test_json.cpp — тести парсера та генератора JSON.
//
// Парсер розбирає дані, які надходять ззовні, тому перевіряється не лише
// коректна робота, а й поведінка на ворожому вході: глибока вкладеність,
// обрізані рядки, некоректні escape-послідовності.

#include "test_framework.h"
#include "json.h"

using namespace cm;
using namespace cm::json;

CM_TEST(json, parses_simple_object) {
    Value root;
    CHECK(Parse(R"({"a":1,"b":"текст","c":true})", root));
    CHECK(root.isObject());
    CHECK_EQ(root["a"].asInt(0), 1);
    CHECK_STR(root["b"].asString(), "текст");
    CHECK(root["c"].asBool(false));
}

CM_TEST(json, parses_nested_structures) {
    Value root;
    CHECK(Parse(R"({"message":{"content":[{"type":"tool_use","name":"Edit"}]}})", root));
    CHECK_STR(root["message"]["content"][0]["name"].asString(), "Edit");
    CHECK_STR(root.path("message.content")[0]["type"].asString(), "tool_use");
}

CM_TEST(json, missing_fields_are_null_not_crash) {
    Value root;
    CHECK(Parse(R"({"a":1})", root));

    // Ланцюжок звернень до неіснуючих полів має бути безпечним:
    // саме на це спирається код розбору транскриптів.
    CHECK(root["немає"]["теж немає"][5]["і це"].isNull());
    CHECK_STR(root["немає"].asStringOr("запасне"), "запасне");
    CHECK_EQ(root["немає"].asInt(42), 42);
}

CM_TEST(json, rejects_malformed_input) {
    Value root;
    CHECK_FALSE(Parse("", root));
    CHECK_FALSE(Parse("{", root));
    CHECK_FALSE(Parse(R"({"a":})", root));
    CHECK_FALSE(Parse(R"({"a" 1})", root));
    CHECK_FALSE(Parse(R"({a:1})", root));
    CHECK_FALSE(Parse("[1,2,", root));
    CHECK_FALSE(Parse(R"({"a":01})", root));       // провідний нуль заборонений
    CHECK_FALSE(Parse(R"({"a":.5})", root));
    CHECK_FALSE(Parse(R"({"a":"незакритий)", root));
    CHECK_FALSE(Parse(R"({"a":1}зайве)", root));   // сміття після документа
}

CM_TEST(json, rejects_excessive_nesting) {
    // Глибока вкладеність — класичний спосіб переповнити стек парсера.
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';

    Value root;
    CHECK_FALSE(Parse(deep, root));
}

CM_TEST(json, accepts_nesting_within_limit) {
    std::string nested;
    for (int i = 0; i < 20; ++i) nested += '[';
    nested += '1';
    for (int i = 0; i < 20; ++i) nested += ']';

    Value root;
    CHECK(Parse(nested, root));
}

CM_TEST(json, handles_escapes) {
    Value root;
    CHECK(Parse(R"({"s":"рядок\nз\tтабуляцією!"})", root));
    CHECK_STR(root["s"].asString(), "рядок\nз\tтабуляцією!");
}

CM_TEST(json, handles_surrogate_pairs) {
    Value root;
    // U+1F600 (усміхнене обличчя) у вигляді сурогатної пари.
    // Записано escape-послідовністю, а не самим символом: MSVC не приймає
    // сурогати у вихідному тексті програми.
    CHECK(Parse("{\"e\":\"\\ud83d\\ude00\"}", root));

    const std::string& emoji = root["e"].asString();
    CHECK_EQ(emoji.size(), 4u);  // 4 байти UTF-8
    CHECK_EQ(static_cast<unsigned char>(emoji[0]), 0xF0u);
    CHECK_EQ(static_cast<unsigned char>(emoji[1]), 0x9Fu);
    CHECK_EQ(static_cast<unsigned char>(emoji[2]), 0x98u);
    CHECK_EQ(static_cast<unsigned char>(emoji[3]), 0x80u);
}

CM_TEST(json, lone_surrogate_becomes_replacement) {
    Value root;
    // Непарний сурогат не має валити розбір: транскрипт із таким
    // символом усе одно варто прочитати.
    CHECK(Parse("{\"e\":\"\\ud83d\"}", root));
    CHECK_EQ(root["e"].asString().size(), 3u);  // U+FFFD
}

CM_TEST(json, rejects_control_characters_in_string) {
    Value root;
    std::string input = R"({"s":"a)";
    input += '\n';
    input += R"("})";
    CHECK_FALSE(Parse(input, root));
}

CM_TEST(json, numbers) {
    Value root;
    CHECK(Parse(R"({"i":42,"n":-17,"f":3.5,"e":1e3,"z":0})", root));
    CHECK_EQ(root["i"].asInt(0), 42);
    CHECK_EQ(root["n"].asInt(0), -17);
    CHECK_EQ(root["e"].asInt(0), 1000);
    CHECK_EQ(root["z"].asInt(-1), 0);
}

CM_TEST(json, large_numbers_do_not_overflow) {
    Value root;
    CHECK(Parse(R"({"big":1e300})", root));
    // Значення поза межами int64 має дати запасне, а не невизначену поведінку.
    CHECK_EQ(root["big"].asInt(-1), -1);
}

CM_TEST(json, unix_timestamps_survive_roundtrip) {
    // Позначки часу — найважливіші числа в протоколі, і вони великі.
    Value root;
    CHECK(Parse(R"({"ts":1788953990322})", root));
    CHECK_EQ(root["ts"].asInt(0), 1788953990322LL);
}

CM_TEST(writer, builds_valid_object) {
    std::string out;
    Writer writer(out);
    writer.beginObject();
    writer.field("type", "status");
    writer.field("seq", static_cast<int64_t>(42));
    writer.field("ok", true);
    writer.endObject();

    Value root;
    CHECK(Parse(out, root));
    CHECK_STR(root["type"].asString(), "status");
    CHECK_EQ(root["seq"].asInt(0), 42);
    CHECK(root["ok"].asBool(false));
}

CM_TEST(writer, escapes_special_characters) {
    std::string out;
    Writer writer(out);
    writer.beginObject();
    writer.field("s", "лапки \" зворотна \\ новий\nрядок");
    writer.endObject();

    Value root;
    CHECK(Parse(out, root));
    CHECK_STR(root["s"].asString(), "лапки \" зворотна \\ новий\nрядок");
}

CM_TEST(writer, nested_arrays_and_objects) {
    std::string out;
    Writer writer(out);
    writer.beginObject();
    writer.key("items");
    writer.beginArray();
    for (int i = 0; i < 3; ++i) {
        writer.beginObject();
        writer.field("i", static_cast<int64_t>(i));
        writer.endObject();
    }
    writer.endArray();
    writer.field("after", "значення");
    writer.endObject();

    Value root;
    CHECK(Parse(out, root));
    CHECK_EQ(root["items"].size(), 3u);
    CHECK_EQ(root["items"][2]["i"].asInt(-1), 2);
    CHECK_STR(root["after"].asString(), "значення");
}

CM_TEST(writer, handles_int64_min) {
    std::string out;
    Writer writer(out);
    writer.beginObject();
    writer.field("min", INT64_MIN);
    writer.endObject();

    Value root;
    CHECK(Parse(out, root));
    // Перевіряємо саме факт коректного запису: модуль INT64_MIN
    // не вміщується в int64, і наївна реалізація тут ламається.
    CHECK(out.find("-9223372036854775808") != std::string::npos);
}

CM_TEST(writer, utf8_passes_through) {
    std::string out;
    Writer writer(out);
    writer.beginObject();
    writer.field("текст", "Привіт, світе");
    writer.endObject();

    Value root;
    CHECK(Parse(out, root));
    CHECK_STR(root["текст"].asString(), "Привіт, світе");
}
