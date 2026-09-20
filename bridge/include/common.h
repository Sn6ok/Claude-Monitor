// common.h — базові типи та утиліти Windows Bridge.
//
// Проєкт свідомо не використовує жодної зовнішньої бібліотеки: усе будується
// на Win32 API та стандартній бібліотеці C++. Причина — вимога мінімального
// споживання пам'яті та відсутності зайвих залежностей
// (Частина 6 §6, §39 Master Prompt).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cm {

// ── Версії ───────────────────────────────────────────────────────────────────

inline constexpr int kProtocolVersion = 1;
inline constexpr wchar_t kBridgeVersion[] = L"2.0.0-beta";
inline constexpr char kBridgeVersionA[] = "2.0.0-beta";

// ── Ліміти ───────────────────────────────────────────────────────────────────
//
// Кожне обмеження існує, щоб жодна структура не могла зрости необмежено
// (Частина 5 §16, §33 Master Prompt). Значення узгоджені з docs/protocol.md §2.

inline constexpr size_t kMaxFrameBytes      = 65536;

/// Скільки тексту вміщує ОДНА подія.
///
/// Це не межа довжини репліки, а розмір частини: довга репліка ділиться
/// на кілька подій, які застосунок склеює назад. Так кадр лишається малим,
/// а текст доходить повністю.
inline constexpr size_t kMaxEventText       = 3072;

/// Скільки тексту передається з одного запису — репліки, виводу команди
/// чи самої команди — сумарно, усіма частинами. Скорочень у звичайній роботі
/// немає: межа лише запобіжна, проти випадкового журналу на мегабайти,
/// який поклав би пам'ять телефона.
inline constexpr size_t kMaxOutputBytes     = 262144;

/// Подій в одному пакеті. Разом із розміром частини це тримає кадр
/// у межах ~24 КБ до шифрування, тобто вдвічі нижче за межу кадру.
inline constexpr size_t kMaxEventsPerBatch  = 8;

/// Ціль дії — шлях, зразок пошуку, адреса чи команда — передається повністю:
/// довга ділиться на частини так само, як текст.
inline constexpr size_t kMaxTargetBytes     = kMaxOutputBytes;

/// Скільки з цілі дії бере картка задачі в переліку: там лише прев'ю,
/// повна ціль — у стрічці.
inline constexpr size_t kMaxActivityPreview = 1024;
inline constexpr size_t kMaxSessions        = 16;
inline constexpr size_t kMaxFinishedHistory = 20;
inline constexpr size_t kMaxQueueEvents     = 512;
inline constexpr size_t kMaxSnapshotEvents  = 20;

/// Текст події у знімку стану — повністю, як і в живих подіях. Щоб знімок
/// вмістився в кадр, Bridge за потреби бере менше подій на задачу,
/// а не скорочує їхній текст.
inline constexpr size_t kMaxSnapshotText    = kMaxEventText;
inline constexpr size_t kMaxLineBytes       = 1u << 20;   // один рядок JSONL
inline constexpr size_t kMaxReadChunk       = 1u << 20;   // читання за раз

// ── Час ──────────────────────────────────────────────────────────────────────

/// Час у мілісекундах від епохи Unix. Для позначок подій.
uint64_t NowUnixMs();

/// Монотонний час у мілісекундах. Для інтервалів і таймаутів: не залежить
/// від переведення системного годинника (Частина 2 §16 Master Prompt).
uint64_t NowMonotonicMs();

/// Момент скидання ліміту за текстом Claude Code: «10:30pm (Europe/Kyiv)»,
/// «Aug 23, 5pm (Europe/Kyiv)». Час тлумачиться як місцевий: Claude Code
/// пише його в часовому поясі цього ж комп'ютера. Береться найближчий такий
/// момент після `messageMs`. 0 — формат не розпізнано.
uint64_t ParseLimitResetMs(std::string_view text, uint64_t messageMs);

// ── Рядки ────────────────────────────────────────────────────────────────────
//
// Внутрішнє представлення — UTF-8. Win32 працює з UTF-16, тому конвертація
// потрібна лише на межі з API. Транскрипти Claude Code — UTF-8, і саме в
// цьому вигляді текст іде далі без зайвих перетворень.

std::string  Utf16ToUtf8(std::wstring_view text);
std::wstring Utf8ToUtf16(std::string_view text);

/// Обрізає рядок до заданої кількості байтів, не розриваючи символ UTF-8.
/// Якщо обрізання сталося, додає позначку « …».
std::string TruncateUtf8(std::string_view text, size_t maxBytes);

/// Прибирає керівні символи, які зламали б JSON або вивід у журнал.
/// Переноси рядка стають пробілами — для назв, шляхів та інших однорядкових
/// значень.
std::string SanitizeText(std::string_view text);

/// Те саме, але зі збереженням розбиття на рядки — для тексту подій.
/// Вивід команди чи репліка зі списком без переносів злипаються в суцільну
/// стрічку, у якій уже не видно ані структури, ані меж пунктів.
std::string SanitizeMultiline(std::string_view text);

bool StartsWith(std::string_view text, std::string_view prefix);
bool EndsWithNoCase(std::wstring_view text, std::wstring_view suffix);
bool ContainsNoCase(std::wstring_view haystack, std::wstring_view needle);

// ── Кодування ────────────────────────────────────────────────────────────────

std::string Base64UrlEncode(const uint8_t* data, size_t length);
std::string Base64UrlEncode(const std::vector<uint8_t>& data);
bool        Base64UrlDecode(std::string_view text, std::vector<uint8_t>& out);
std::string HexEncode(const uint8_t* data, size_t length);

// ── Обгортки над дескрипторами ───────────────────────────────────────────────
//
// Ручне закриття дескрипторів — джерело витоків, особливо на шляхах помилок.
// Ці типи гарантують звільнення в усіх випадках.

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); h_ = other.h_; other.h_ = nullptr; }
        return *this;
    }

    void reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) ::CloseHandle(h_);
        h_ = h;
    }

    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return valid(); }

    HANDLE release() { HANDLE h = h_; h_ = nullptr; return h; }

private:
    HANDLE h_ = nullptr;
};

/// Критична секція з RAII-блокуванням. Легша за std::mutex на Windows
/// і не тягне залежностей від реалізації стандартної бібліотеки.
class Lock {
public:
    Lock()  { ::InitializeCriticalSectionAndSpinCount(&cs_, 1000); }
    ~Lock() { ::DeleteCriticalSection(&cs_); }

    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void enter() { ::EnterCriticalSection(&cs_); }
    void leave() { ::LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_{};
};

class Guard {
public:
    explicit Guard(Lock& lock) : lock_(lock) { lock_.enter(); }
    ~Guard() { lock_.leave(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    Lock& lock_;
};

// ── Шляхи ────────────────────────────────────────────────────────────────────

/// Каталог %USERPROFILE%\.claude
std::wstring ClaudeHomeDir();

/// Каталог %USERPROFILE%\.claude-monitor — власні дані Bridge
std::wstring BridgeDataDir();

/// Створює каталог разом із проміжними. true, якщо каталог існує після виклику.
bool EnsureDirectory(const std::wstring& path);

bool FileExists(const std::wstring& path);
bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes = 1u << 20);
bool WriteWholeFileAtomic(const std::wstring& path, std::string_view data);

}  // namespace cm
