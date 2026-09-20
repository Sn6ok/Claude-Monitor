// test_framework.h — мінімальний каркас для юніт-тестів.
//
// Замість повноцінної бібліотеки (GoogleTest, Catch2) використовується
// сотня рядків власного коду. Причина та сама, що й у решті проєкту:
// не тягнути залежність заради того, що робиться кількома десятками
// рядків (Частина 6 §39 Master Prompt).

#pragma once

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace cm::test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

/// Реєстр тестів. Тести самореєструються через статичні об'єкти.
inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body) {
        Registry().push_back({suite, name, std::move(body)});
    }
};

/// Лічильники поточного тесту.
inline int& CurrentFailures() {
    static int failures = 0;
    return failures;
}

inline int& TotalChecks() {
    static int checks = 0;
    return checks;
}

inline void ReportFailure(const char* file, int line, const std::string& message) {
    CurrentFailures() += 1;
    std::printf("      %s:%d\n        %s\n", file, line, message.c_str());
}

// ── Перевірки ────────────────────────────────────────────────────────────────

inline void CheckTrue(bool value, const char* expression, const char* file, int line) {
    TotalChecks() += 1;
    if (!value) ReportFailure(file, line, std::string("хибно: ") + expression);
}

inline void CheckEqualStr(const std::string& actual, const std::string& expected,
                          const char* expression, const char* file, int line) {
    TotalChecks() += 1;
    if (actual != expected) {
        ReportFailure(file, line,
                      std::string(expression) + "\n        отримано:  \"" + actual +
                      "\"\n        очікувано: \"" + expected + "\"");
    }
}

template <typename A, typename B>
void CheckEqualNum(A actual, B expected, const char* expression, const char* file, int line) {
    TotalChecks() += 1;
    if (!(actual == static_cast<A>(expected))) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "%s\n        отримано:  %lld\n        очікувано: %lld",
                      expression,
                      static_cast<long long>(actual),
                      static_cast<long long>(expected));
        ReportFailure(file, line, buffer);
    }
}

int RunAll();

}  // namespace cm::test

// ── Макроси ──────────────────────────────────────────────────────────────────

#define CM_TEST(suite_name, test_name)                                            \
    static void suite_name##_##test_name##_body();                                \
    static ::cm::test::Registrar suite_name##_##test_name##_registrar(            \
        #suite_name, #test_name, suite_name##_##test_name##_body);                \
    static void suite_name##_##test_name##_body()

#define CHECK(expr) ::cm::test::CheckTrue((expr), #expr, __FILE__, __LINE__)

#define CHECK_FALSE(expr) ::cm::test::CheckTrue(!(expr), "не " #expr, __FILE__, __LINE__)

#define CHECK_STR(actual, expected) \
    ::cm::test::CheckEqualStr((actual), (expected), #actual, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected) \
    ::cm::test::CheckEqualNum((actual), (expected), #actual, __FILE__, __LINE__)
