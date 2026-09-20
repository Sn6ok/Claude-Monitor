// common.cpp — реалізація базових утиліт.

#include "common.h"

#include <shlobj.h>

#include <algorithm>

namespace cm {

// ── Час ──────────────────────────────────────────────────────────────────────

uint64_t NowUnixMs() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER value{};
    value.LowPart  = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;

    // FILETIME рахує 100-наносекундні інтервали від 1601-01-01,
    // Unix-час — секунди від 1970-01-01. Різниця — стала нижче.
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    return (value.QuadPart - kEpochDiff100ns) / 10000ULL;
}

uint64_t NowMonotonicMs() {
    // GetTickCount64 не залежить від змін системного годинника і не
    // переповнюється (на відміну від 32-бітного GetTickCount).
    return ::GetTickCount64();
}

// ── Рядки ────────────────────────────────────────────────────────────────────

std::string Utf16ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};

    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};

    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToUtf16(std::string_view text) {
    if (text.empty()) return {};

    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};

    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), needed);
    return out;
}

std::string TruncateUtf8(std::string_view text, size_t maxBytes) {
    if (text.size() <= maxBytes) return std::string(text);
    if (maxBytes == 0) return {};

    // Відступаємо назад до початку символу: продовження UTF-8 має
    // старші біти 10xxxxxx. Розрив посередині символу дав би
    // некоректний UTF-8 і зламав би JSON на приймачі.
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }

    std::string out(text.substr(0, cut));
    out += " \xE2\x80\xA6";  // U+2026, три крапки одним символом
    return out;
}

namespace {

/// Прибирає керівні символи. `keepLines` вирішує долю переносів рядка:
/// у назві задачі перенос лише зламав би верстку, а у виводі команди
/// саме він і несе структуру.
std::string StripControlChars(std::string_view text, bool keepLines) {
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (c == '\r') continue;
        if (c == '\n') { out += keepLines ? '\n' : ' '; continue; }

        // Табуляцію розгортаємо в пробіли: її ширина на телефоні
        // непередбачувана, а вирівнювання виводу від неї залежить.
        if (c == '\t') { out.append(keepLines ? 4u : 1u, ' '); continue; }

        // Інші керівні символи прибираємо: вони не несуть змісту в UI,
        // але можуть заплутати термінал або приймач.
        if (c < 0x20 || c == 0x7F) continue;

        out += static_cast<char>(c);
    }
    return out;
}

}  // namespace

std::string SanitizeText(std::string_view text) {
    const std::string out = StripControlChars(text, /*keepLines=*/false);

    // Стискаємо послідовні пробіли — вивід інструментів рясніє відступами,
    // які в компактному UI телефона лише марнують місце.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prevSpace = false;
    for (char c : out) {
        const bool isSpace = (c == ' ');
        if (isSpace && prevSpace) continue;
        collapsed += c;
        prevSpace = isSpace;
    }

    const size_t first = collapsed.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const size_t last = collapsed.find_last_not_of(' ');
    return collapsed.substr(first, last - first + 1);
}

std::string SanitizeMultiline(std::string_view text) {
    const std::string cleaned = StripControlChars(text, /*keepLines=*/true);

    // Пробіли всередині рядка лишаються недоторканими: у виводі команд
    // ними вирівняні колонки, і без них таблиця перетворюється на кашу.
    // Прибираються лише хвостові пробіли та зайві порожні рядки.
    std::string out;
    out.reserve(cleaned.size());

    size_t pos = 0;
    bool sawBlank = false;
    bool wroteAnything = false;

    for (;;) {
        const size_t end = cleaned.find('\n', pos);
        const size_t stop = (end == std::string::npos) ? cleaned.size() : end;

        std::string_view line(cleaned.data() + pos, stop - pos);
        while (!line.empty() && line.back() == ' ') line.remove_suffix(1);

        if (line.empty()) {
            // Порожні рядки на початку не мають чого відділяти.
            if (wroteAnything) sawBlank = true;
        } else {
            if (wroteAnything) {
                out += '\n';
                // Скільки б порожніх рядків не було поспіль, лишається один:
                // абзаци розділені, але екран не марнується.
                if (sawBlank) out += '\n';
            }
            out.append(line);
            wroteAnything = true;
            sawBlank = false;
        }

        if (end == std::string::npos) break;
        pos = end + 1;
    }

    return out;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWithNoCase(std::wstring_view text, std::wstring_view suffix) {
    if (text.size() < suffix.size()) return false;
    const std::wstring_view tail = text.substr(text.size() - suffix.size());
    return ::CompareStringOrdinal(tail.data(), static_cast<int>(tail.size()),
                                  suffix.data(), static_cast<int>(suffix.size()),
                                  TRUE) == CSTR_EQUAL;
}

bool ContainsNoCase(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;

    const size_t limit = haystack.size() - needle.size();
    for (size_t i = 0; i <= limit; ++i) {
        if (::CompareStringOrdinal(haystack.data() + i, static_cast<int>(needle.size()),
                                   needle.data(), static_cast<int>(needle.size()),
                                   TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

// ── Кодування ────────────────────────────────────────────────────────────────

namespace {
constexpr char kB64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}

std::string Base64UrlEncode(const uint8_t* data, size_t length) {
    std::string out;
    out.reserve(((length + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < length; i += 3) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8) |
                                 static_cast<uint32_t>(data[i + 2]);
        out += kB64UrlAlphabet[(triple >> 18) & 0x3F];
        out += kB64UrlAlphabet[(triple >> 12) & 0x3F];
        out += kB64UrlAlphabet[(triple >> 6) & 0x3F];
        out += kB64UrlAlphabet[triple & 0x3F];
    }

    // base64url без доповнення '=' — саме такий формат очікує протокол.
    if (i + 1 == length) {
        const uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        out += kB64UrlAlphabet[(triple >> 18) & 0x3F];
        out += kB64UrlAlphabet[(triple >> 12) & 0x3F];
    } else if (i + 2 == length) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8);
        out += kB64UrlAlphabet[(triple >> 18) & 0x3F];
        out += kB64UrlAlphabet[(triple >> 12) & 0x3F];
        out += kB64UrlAlphabet[(triple >> 6) & 0x3F];
    }

    return out;
}

std::string Base64UrlEncode(const std::vector<uint8_t>& data) {
    return Base64UrlEncode(data.data(), data.size());
}

bool Base64UrlDecode(std::string_view text, std::vector<uint8_t>& out) {
    auto decodeChar = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };

    out.clear();
    out.reserve((text.size() * 3) / 4 + 3);

    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        const int value = decodeChar(c);
        if (value < 0) return false;  // сторонній символ — вхід некоректний

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string HexEncode(const uint8_t* data, size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out += kHex[(data[i] >> 4) & 0x0F];
        out += kHex[data[i] & 0x0F];
    }
    return out;
}

// ── Шляхи ────────────────────────────────────────────────────────────────────

namespace {

std::wstring UserProfileDir() {
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &raw))) {
        std::wstring result(raw);
        ::CoTaskMemFree(raw);
        return result;
    }

    // Резервний шлях через змінну середовища — на випадок, якщо
    // SHGetKnownFolderPath недоступна в обмеженому середовищі.
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) return std::wstring(buffer, length);

    return {};
}

}  // namespace

std::wstring ClaudeHomeDir() {
    const std::wstring profile = UserProfileDir();
    if (profile.empty()) return {};
    return profile + L"\\.claude";
}

std::wstring BridgeDataDir() {
    const std::wstring profile = UserProfileDir();
    if (profile.empty()) return {};
    return profile + L"\\.claude-monitor";
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;

    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // Створюємо проміжні каталоги знизу вгору.
    const size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos && separator > 2) {
        EnsureDirectory(path.substr(0, separator));
    }

    if (::CreateDirectoryW(path.c_str(), nullptr)) return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes) {
    out.clear();

    // FILE_SHARE_* із повним набором: Claude Code активно пише в ці файли,
    // і Bridge не має права заважати йому — навіть тимчасовим блокуванням
    // (Частина 5 §2 Master Prompt).
    Handle file(::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) return false;
    if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > maxBytes) return false;

    out.resize(static_cast<size_t>(size.QuadPart));
    if (out.empty()) return true;

    DWORD read = 0;
    if (!::ReadFile(file.get(), out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
        out.clear();
        return false;
    }
    out.resize(read);
    return true;
}

bool WriteWholeFileAtomic(const std::wstring& path, std::string_view data) {
    const std::wstring temp = path + L".tmp";

    {
        Handle file(::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) return false;

        DWORD written = 0;
        if (!::WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()),
                         &written, nullptr) || written != data.size()) {
            return false;
        }
        ::FlushFileBuffers(file.get());
    }

    // Заміна одним кроком: обрив живлення посеред запису не залишає
    // напівзаписаного файлу.
    if (::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;

    ::DeleteFileW(temp.c_str());
    return false;
}

// ── Час скидання ліміту ──────────────────────────────────────────────────────

namespace {

uint64_t ResetFileTimeToMs(const FILETIME& ft) {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (value.QuadPart < kEpochDiff100ns) return 0;
    return (value.QuadPart - kEpochDiff100ns) / 10000ULL;
}

FILETIME ResetMsToFileTime(uint64_t ms) {
    ULARGE_INTEGER value{};
    value.QuadPart = ms * 10000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    return ft;
}

/// Місцевий час → мс Unix. 0 — такої дати не існує (скажімо, 30 лютого).
uint64_t ResetLocalToMs(const SYSTEMTIME& local) {
    SYSTEMTIME utc{};
    if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return 0;
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&utc, &ft)) return 0;
    return ResetFileTimeToMs(ft);
}

}  // namespace

uint64_t ParseLimitResetMs(std::string_view text, uint64_t messageMs) {
    if (messageMs == 0 || text.empty()) return 0;

    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    const auto isLetter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };

    size_t i = 0;
    const auto skipSpaces = [&]() {
        while (i < text.size() && text[i] == ' ') ++i;
    };
    const auto readNumber = [&](int maxDigits) {
        int value = 0;
        int digits = 0;
        while (i < text.size() && digits < maxDigits && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        return digits > 0 ? value : -1;
    };

    // Необов'язкова дата: «Aug 23, ».
    int month = 0;
    int day = 0;
    if (isLetter(text[0])) {
        static constexpr std::string_view kMonths[] = {
            "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
        };
        if (text.size() < 3) return 0;
        const char prefix[3] = {lower(text[0]), lower(text[1]), lower(text[2])};
        for (int m = 0; m < 12; ++m) {
            if (std::string_view(prefix, 3) == kMonths[m]) month = m + 1;
        }
        if (month == 0) return 0;
        while (i < text.size() && isLetter(text[i])) ++i;
        skipSpaces();
        day = readNumber(2);
        if (day < 1 || day > 31) return 0;
        if (i < text.size() && text[i] == ',') ++i;
        skipSpaces();
    }

    // Час: «10:30pm» чи «5pm».
    int hour = readNumber(2);
    if (hour < 1 || hour > 12) return 0;
    int minute = 0;
    if (i < text.size() && text[i] == ':') {
        ++i;
        minute = readNumber(2);
        if (minute < 0 || minute > 59) return 0;
    }
    skipSpaces();
    if (i + 2 > text.size()) return 0;
    const char meridiem = lower(text[i]);
    if (lower(text[i + 1]) != 'm' || (meridiem != 'a' && meridiem != 'p')) return 0;
    hour = hour % 12 + (meridiem == 'p' ? 12 : 0);

    // Повідомлення про ліміт — у місцевий час цього комп'ютера.
    const FILETIME messageFt = ResetMsToFileTime(messageMs);
    SYSTEMTIME messageUtc{};
    SYSTEMTIME target{};
    if (!::FileTimeToSystemTime(&messageFt, &messageUtc) ||
        !::SystemTimeToTzSpecificLocalTime(nullptr, &messageUtc, &target)) {
        return 0;
    }

    target.wHour = static_cast<WORD>(hour);
    target.wMinute = static_cast<WORD>(minute);
    target.wSecond = 0;
    target.wMilliseconds = 0;
    target.wDayOfWeek = 0;
    if (month != 0) {
        target.wMonth = static_cast<WORD>(month);
        target.wDay = static_cast<WORD>(day);
    }

    uint64_t reset = ResetLocalToMs(target);
    if (reset > messageMs) return reset;
    if (reset == 0 && month == 0) return 0;

    // Такий момент уже минув — отже, мається на увазі наступний: завтра
    // (для часу без дати) або наступного року (для дати). Доба додається
    // в місцевому часі, щоб перехід на літній час не зсунув годину.
    if (month != 0) {
        target.wYear = static_cast<WORD>(target.wYear + 1);
    } else {
        FILETIME asNumber{};
        if (!::SystemTimeToFileTime(&target, &asNumber)) return 0;
        ULARGE_INTEGER shifted{};
        shifted.LowPart = asNumber.dwLowDateTime;
        shifted.HighPart = asNumber.dwHighDateTime;
        shifted.QuadPart += 24ULL * 3600ULL * 10000000ULL;
        asNumber.dwLowDateTime = shifted.LowPart;
        asNumber.dwHighDateTime = shifted.HighPart;
        if (!::FileTimeToSystemTime(&asNumber, &target)) return 0;
    }

    reset = ResetLocalToMs(target);
    return reset > messageMs ? reset : 0;
}

}  // namespace cm
