// watcher.h — спостереження за файловою системою без опитування.
//
// Це той механізм, завдяки якому Bridge у стані спокою споживає нуль CPU
// (Частина 5 §31, §32 Master Prompt). Замість циклу «перевір і поспи»
// потік блокується на об'єкті ядра і прокидається лише тоді, коли Windows
// повідомляє про фактичну зміну у відстежуваному каталозі.
//
// Відстежуються два каталоги:
//   ~/.claude/sessions          поява та зникнення сесій
//   ~/.claude/projects          дописування транскриптів
//
// Читання цих каталогів жодним чином не заважає Claude Code: файли
// відкриваються лише на читання, з повним набором прапорців спільного доступу.

#pragma once

#include "common.h"

#include <functional>
#include <vector>

namespace cm {

/// Причина пробудження.
enum class WatchEvent : uint8_t {
    SessionsChanged = 0,  ///< змінився каталог реєстру сесій
    TranscriptChanged,    ///< змінився якийсь транскрипт
    Shutdown,             ///< надійшов сигнал завершення
    Timeout,              ///< спрацював запобіжний таймер
};

/// Спостерігач за одним каталогом. Використовує асинхронний
/// ReadDirectoryChangesW із подією завершення.
class DirectoryWatcher {
public:
    DirectoryWatcher() = default;
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    /// @param path      каталог для спостереження
    /// @param recursive чи стежити за підкаталогами
    bool Start(const std::wstring& path, bool recursive);

    /// Дескриптор події для очікування в WaitForMultipleObjects.
    HANDLE waitHandle() const { return overlapped_.hEvent; }

    /// Підтверджує спрацювання та ставить наступний запит.
    /// @returns true, якщо спостереження триває
    bool Consume();

    void Stop();

    bool active() const { return directory_.valid(); }
    const std::wstring& path() const { return path_; }

private:
    bool Queue();

    std::wstring path_;
    Handle       directory_;
    OVERLAPPED   overlapped_{};
    Handle       event_;
    bool         recursive_ = false;

    /// Буфер повідомлень від ядра. Вміст навмисно не розбирається:
    /// Bridge цікавить лише сам факт зміни, після якого він дочитує файли
    /// з відомих зміщень. Це надійніше за довіру до списку імен, який
    /// при переповненні буфера ядро може віддати порожнім.
    std::vector<uint8_t> buffer_;
};

/// Об'єднує обидва спостерігачі та сигнали завершення в одне очікування.
class WatchSet {
public:
    WatchSet();
    ~WatchSet();

    bool Start(const std::wstring& sessionsDir, const std::wstring& projectsDir);

    /// Блокується до появи будь-якої події.
    ///
    /// @param timeoutMs запобіжний таймер. Потрібен не для опитування,
    ///        а на випадок, коли повідомлення файлової системи загубилося
    ///        (таке буває на мережевих дисках і при переповненні черги ядра).
    ///        Значення велике, тож на споживання CPU це не впливає.
    WatchEvent Wait(uint32_t timeoutMs);

    /// Сигналізує про завершення роботи з іншого потоку.
    void Shutdown();

    /// Додатковий дескриптор, який теж пробуджує очікування —
    /// використовується для завершення процесу Claude Desktop.
    void AddExternalHandle(HANDLE handle);

    /// Забуває зовнішній дескриптор: процес завершився, а Bridge працює далі.
    /// Без цього очікування миттєво поверталося б на вже сигнальному дескрипторі.
    void ClearExternalHandle();

    bool externalSignalled() const { return externalSignalled_; }

private:
    DirectoryWatcher sessions_;
    DirectoryWatcher projects_;
    Handle           shutdownEvent_;
    HANDLE           external_ = nullptr;
    bool             externalSignalled_ = false;
};

}  // namespace cm
