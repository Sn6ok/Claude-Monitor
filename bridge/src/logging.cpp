// logging.cpp — реалізація журналювання.

#include "logging.h"

#include <cstdarg>
#include <cstdio>

namespace cm {

const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "error";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Info:  return "info";
        case LogLevel::Debug: return "debug";
    }
    return "info";
}

LogLevel ParseLogLevel(std::string_view text) {
    if (text == "error") return LogLevel::Error;
    if (text == "warn")  return LogLevel::Warn;
    if (text == "debug") return LogLevel::Debug;
    return LogLevel::Info;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::Configure(const std::wstring& path, LogLevel level, bool console) {
    Guard guard(lock_);

    level_ = level;
    console_ = console;
    path_ = path;
    file_.reset();
    bytesWritten_ = 0;

    if (path_.empty()) return;

    const size_t separator = path_.find_last_of(L"\\/");
    if (separator != std::wstring::npos) EnsureDirectory(path_.substr(0, separator));

    file_.reset(::CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));

    if (file_) {
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file_.get(), &size)) {
            bytesWritten_ = static_cast<uint64_t>(size.QuadPart);
        }
    }
}

void Logger::RotateIfNeeded() {
    if (bytesWritten_ < kMaxLogBytes || path_.empty()) return;

    file_.reset();

    // Один запасний файл: попередній перезаписується. Історія журналу
    // не має цінності, а місце на диску має.
    const std::wstring backup = path_ + L".1";
    ::DeleteFileW(backup.c_str());
    ::MoveFileW(path_.c_str(), backup.c_str());

    file_.reset(::CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    bytesWritten_ = 0;
}

void Logger::Write(LogLevel level, std::string_view message) {
    if (!enabled(level)) return;

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    char header[64];
    const int headerLength = std::snprintf(
        header, sizeof(header), "%02d:%02d:%02d.%03d [%s] ",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, ToString(level));

    Guard guard(lock_);

    if (console_) {
        std::fputs(header, stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
    }

    if (!file_) return;

    RotateIfNeeded();
    if (!file_) return;

    DWORD written = 0;
    ::WriteFile(file_.get(), header, static_cast<DWORD>(headerLength), &written, nullptr);
    bytesWritten_ += written;

    ::WriteFile(file_.get(), message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
    bytesWritten_ += written;

    ::WriteFile(file_.get(), "\r\n", 2, &written, nullptr);
    bytesWritten_ += written;

    // FlushFileBuffers навмисно не викликається на кожен запис: примусовий
    // скид на диск щоразу створював би постійний дисковий ввід-вивід,
    // що прямо заборонено Частиною 5 §34 Master Prompt.
}

namespace {

void FormatAndWrite(Logger& logger, LogLevel level, const char* format, va_list args) {
    if (!logger.enabled(level)) return;

    char buffer[1024];
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    if (length < 0) return;

    const size_t safeLength =
        (static_cast<size_t>(length) < sizeof(buffer)) ? static_cast<size_t>(length)
                                                       : sizeof(buffer) - 1;
    logger.Write(level, std::string_view(buffer, safeLength));
}

}  // namespace

void Logger::Infof(const char* format, ...) {
    va_list args; va_start(args, format);
    FormatAndWrite(*this, LogLevel::Info, format, args);
    va_end(args);
}

void Logger::Warnf(const char* format, ...) {
    va_list args; va_start(args, format);
    FormatAndWrite(*this, LogLevel::Warn, format, args);
    va_end(args);
}

void Logger::Errorf(const char* format, ...) {
    va_list args; va_start(args, format);
    FormatAndWrite(*this, LogLevel::Error, format, args);
    va_end(args);
}

void Logger::Debugf(const char* format, ...) {
    va_list args; va_start(args, format);
    FormatAndWrite(*this, LogLevel::Debug, format, args);
    va_end(args);
}

}  // namespace cm
