// sessions.h — виявлення активних сесій Claude Code.
//
// Джерело: `~/.claude/sessions/<pid>.json`. Claude Code створює такий файл
// на кожну сесію незалежно від Bridge; читання не впливає на його роботу.
// Формат перевірено емпірично, див. docs/research.md §3.1.
//
// Bridge НЕ використовує поле messagingSocketPath із цих файлів: це
// недокументований внутрішній канал Claude Code, а Частина 5 §11 Master Prompt
// вимагає уникати таких механізмів за наявності альтернативи.

#pragma once

#include "events.h"

#include <string>
#include <vector>

namespace cm {

/// Те, що вдалося прочитати про сесію з реєстру.
struct DiscoveredSession {
    std::string  sessionId;
    uint32_t     pid = 0;
    std::string  cwd;
    std::string  name;         ///< автоматична назва від Claude Code
    uint64_t     startedAtMs = 0;
    std::string  version;
    std::string  entrypoint;
    std::wstring transcriptPath;
};

class SessionScanner {
public:
    SessionScanner();

    /// Каталог, за яким стежить спостерігач файлової системи.
    const std::wstring& sessionsDir() const { return sessionsDir_; }
    const std::wstring& projectsDir() const { return projectsDir_; }
    bool available() const { return !sessionsDir_.empty(); }

    /// Зчитує всі файли реєстру та повертає сесії з живими процесами.
    /// Мертві записи відкидаються: Claude Code не завжди встигає прибрати
    /// свій файл після аварійного завершення.
    std::vector<DiscoveredSession> Scan() const;

    /// Перевіряє, чи справді існує процес із таким PID і чи це Claude Code.
    /// Одного лише існування PID недостатньо: номери процесів переви́користовуються.
    static bool IsSessionProcessAlive(uint32_t pid, uint64_t startedAtMs);

    /// Обчислює ім'я каталогу проєкту за робочим каталогом сесії.
    /// Правило виведено з реальних імен каталогів (docs/research.md §3.2):
    /// кожен символ поза [A-Za-z0-9-] замінюється на дефіс.
    static std::wstring SlugForPath(std::string_view cwd);

    /// Шукає файл транскрипту сесії. Спершу за обчисленим ім'ям каталогу,
    /// далі — прямим пошуком по всіх каталогах проєктів.
    std::wstring FindTranscript(const std::string& sessionId, std::string_view cwd) const;

private:
    bool ParseSessionFile(const std::wstring& path, DiscoveredSession& out) const;

    std::wstring claudeHome_;
    std::wstring sessionsDir_;
    std::wstring projectsDir_;
};

}  // namespace cm
