// lifecycle.h — прив'язка життєвого циклу Bridge до Claude Desktop.
//
// Ключові вимоги Master Prompt (Частина 5 §3, §4, §7, §8):
//   * Bridge НЕ запускається разом із Windows;
//   * Bridge стартує, коли починається сесія Claude Code (через hook);
//   * Bridge завершується, коли закривається Claude Desktop;
//   * одночасно працює рівно один екземпляр;
//   * очікування завершення Claude Desktop не витрачає процесорного часу.
//
// Для останнього використовується дескриптор процесу як об'єкт очікування:
// потік блокується в ядрі й прокидається рівно в момент завершення процесу.
// Жодного опитування «чи ще живий» із інтервалом.

#pragma once

#include "common.h"

namespace cm {

/// Гарантія єдиного екземпляра через іменований м'ютекс.
///
/// М'ютекс створюється в просторі імен Local\, тобто діє в межах сеансу
/// користувача. Це правильніше за Global\: у кожного користувача Windows
/// свій Claude Desktop, тож і свій Bridge.
class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();

    /// @returns true, якщо цей процес став єдиним екземпляром
    bool Acquire();

    /// Чи вже працює інший екземпляр.
    bool alreadyRunning() const { return alreadyRunning_; }

private:
    Handle mutex_;
    bool   alreadyRunning_ = false;
};

/// Відомості про знайдений процес Claude Desktop.
struct ClaudeDesktopInfo {
    uint32_t pid = 0;
    std::wstring executablePath;
    bool found = false;
};

/// Пошук та відстеження процесу Claude Desktop.
class DesktopMonitor {
public:
    /// Знаходить кореневий процес Claude Desktop.
    ///
    /// Ім'я процесу ненадійне: усі процеси застосунку називаються claude.exe,
    /// а серед них є renderer, gpu, utility та інші (docs/research.md §1).
    /// Тому кореневим вважається процес, чий шлях вказує на встановлений
    /// застосунок і чий командний рядок НЕ містить "--type=" — саме так
    /// Chromium позначає дочірні процеси.
    static ClaudeDesktopInfo Find();

    /// Відкриває дескриптор процесу для очікування завершення.
    /// Запитуються мінімальні права: SYNCHRONIZE для очікування
    /// і QUERY_LIMITED_INFORMATION для перевірки стану. Жодних
    /// привілейованих прав (Частина 5 §25 Master Prompt).
    static Handle OpenForWait(uint32_t pid);

    /// Чи живий процес із цим ідентифікатором.
    static bool IsAlive(HANDLE process);
};

}  // namespace cm
