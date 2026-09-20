// test_main.cpp — запуск усіх зареєстрованих тестів.

#include "test_framework.h"
#include "common.h"

namespace cm::test {

int RunAll() {
    ::SetConsoleOutputCP(CP_UTF8);

    auto& registry = Registry();
    std::printf("Тестів зареєстровано: %zu\n\n", registry.size());

    std::string currentSuite;
    int passed = 0;
    int failed = 0;

    for (const TestCase& testCase : registry) {
        if (testCase.suite != currentSuite) {
            currentSuite = testCase.suite;
            std::printf("[%s]\n", currentSuite.c_str());
        }

        CurrentFailures() = 0;

        // Виняток у тесті не має валити весь запуск: решта тестів
        // усе одно варта виконання.
        try {
            testCase.body();
        } catch (const std::exception& error) {
            ReportFailure(__FILE__, __LINE__, std::string("виняток: ") + error.what());
        } catch (...) {
            ReportFailure(__FILE__, __LINE__, "невідомий виняток");
        }

        if (CurrentFailures() == 0) {
            std::printf("  OK    %s\n", testCase.name.c_str());
            ++passed;
        } else {
            std::printf("  ПРОВАЛ %s\n", testCase.name.c_str());
            ++failed;
        }
    }

    std::printf("\n────────────────────────────────\n");
    std::printf("Перевірок: %d\n", TotalChecks());
    std::printf("Пройдено:  %d\n", passed);
    std::printf("Провалено: %d\n", failed);

    return failed == 0 ? 0 : 1;
}

}  // namespace cm::test

int main() {
    return cm::test::RunAll();
}
