// logging.h — мінімальне журналювання Bridge.
//
// Вимоги Master Prompt (Частина 2 §17, Частина 3 §24, Частина 5 §34):
//   * у робочому режимі — мінімум записів;
//   * ніколи не писати ключі, токени, коди pairing, вміст подій;
//   * не писати на диск на кожну подію Claude Code;
//   * розмір файлу журналу обмежений і ротується.

#pragma once

#include "common.h"

namespace cm {

enum class LogLevel : uint8_t { Error = 0, Warn, Info, Debug };

class Logger {
public:
    static Logger& instance();

    /// @param path   файл журналу; порожній рядок вимикає запис на диск
    /// @param level  поріг рівня
    /// @param console дублювати вивід у консоль
    void Configure(const std::wstring& path, LogLevel level, bool console);

    void Write(LogLevel level, std::string_view message);

    void Error(std::string_view message) { Write(LogLevel::Error, message); }
    void Warn(std::string_view message)  { Write(LogLevel::Warn, message); }
    void Info(std::string_view message)  { Write(LogLevel::Info, message); }
    void Debug(std::string_view message) { Write(LogLevel::Debug, message); }

    /// Форматований запис. Навмисно приймає лише прості типи:
    /// це утруднює випадкове потрапляння структур із секретами в журнал.
    void Infof(const char* format, ...);
    void Warnf(const char* format, ...);
    void Errorf(const char* format, ...);
    void Debugf(const char* format, ...);

    LogLevel level() const { return level_; }
    bool enabled(LogLevel level) const {
        return static_cast<uint8_t>(level) <= static_cast<uint8_t>(level_);
    }

private:
    Logger() = default;

    void RotateIfNeeded();

    Lock         lock_;
    std::wstring path_;
    LogLevel     level_ = LogLevel::Info;
    bool         console_ = false;
    Handle       file_;
    uint64_t     bytesWritten_ = 0;

    /// Понад цю межу файл ротується. Один запасний файл — цього досить
    /// для діагностики й не дає журналу з'їдати диск.
    static constexpr uint64_t kMaxLogBytes = 1u * 1024 * 1024;
};

inline Logger& Log() { return Logger::instance(); }

LogLevel ParseLogLevel(std::string_view text);
const char* ToString(LogLevel level);

}  // namespace cm
