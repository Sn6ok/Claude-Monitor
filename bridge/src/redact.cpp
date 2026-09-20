// redact.cpp — консервативне маскування секретів.
//
// Принцип, якого дотримується кожне правило: спрацьовувати лише за наявності
// ЯВНОГО контексту. Правило «рядок із 32 шістнадцяткових символів — це секрет»
// зіпсувало б хеші комітів, контрольні суми й ідентифікатори, тому таких
// правил тут немає.

#include "redact.h"
#include "common.h"

#include <array>
#include <cctype>

namespace cm {

namespace {

constexpr std::string_view kMask = "«приховано»";

bool IsAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsSecretValueChar(char c) {
    // Символи, з яких складаються токени. Пробіл і лапки завершують значення.
    return (c >= '!' && c <= '~') && c != '"' && c != '\'' && c != ',' && c != ';';
}

char LowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Порівняння без урахування регістру для ASCII.
bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    }
    return true;
}

/// Назви змінних, після яких значення вважається секретом.
/// Список навмисно короткий і конкретний.
constexpr std::array<std::string_view, 16> kSecretNames{{
    "password", "passwd", "secret", "token", "api_key", "apikey",
    "access_key", "secret_key", "private_key", "auth_token",
    "client_secret", "credential", "passphrase", "session_key",
    "bearer", "authorization",
}};

bool IsSecretName(std::string_view name) {
    for (std::string_view candidate : kSecretNames) {
        if (EqualsNoCase(name, candidate)) return true;
    }
    return false;
}

/// Префікси токенів відомих сервісів. Ці формати однозначні: рядок,
/// що починається з "sk-" і має достатню довжину, реальним текстом не буває.
struct KnownPrefix {
    std::string_view prefix;
    size_t           minLength;
};

constexpr std::array<KnownPrefix, 10> kKnownPrefixes{{
    {"sk-ant-", 24},   // ключі Anthropic
    {"sk-", 24},       // ключі формату OpenAI
    {"ghp_", 36},      // персональні токени GitHub
    {"gho_", 36},
    {"ghs_", 36},
    {"github_pat_", 40},
    {"AKIA", 20},      // ідентифікатори ключів AWS
    {"ASIA", 20},
    {"xoxb-", 24},     // токени ботів Slack
    {"xoxp-", 24},
}};

/// Читає назву змінної, що стоїть безпосередньо перед позицією `pos`.
/// Повертає діапазон [start, pos) або порожній вигляд.
std::string_view ReadNameBefore(std::string_view text, size_t pos) {
    size_t end = pos;
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;

    size_t start = end;
    while (start > 0) {
        const char c = text[start - 1];
        if (IsAsciiAlpha(c) || (c >= '0' && c <= '9') || c == '_' || c == '-') --start;
        else break;
    }
    if (start == end) return {};
    return text.substr(start, end - start);
}

}  // namespace

bool IsSensitivePath(std::string_view path) {
    // Порівнюємо за назвою файлу, а не за повним шляхом: каталог проєкту
    // може випадково містити слово "secret" без жодного стосунку до секретів.
    size_t slash = path.find_last_of("/\\");
    std::string_view name = (slash == std::string_view::npos) ? path : path.substr(slash + 1);

    static constexpr std::array<std::string_view, 9> kNames{{
        ".env", ".env.local", ".env.production", "id_rsa", "id_ed25519",
        ".npmrc", ".pypirc", "credentials", ".htpasswd",
    }};

    for (std::string_view candidate : kNames) {
        if (EqualsNoCase(name, candidate)) return true;
    }

    // Розширення ключів і сертифікатів.
    static constexpr std::array<std::string_view, 5> kExtensions{{
        ".pem", ".key", ".pfx", ".p12", ".jks",
    }};
    for (std::string_view ext : kExtensions) {
        if (name.size() > ext.size() &&
            EqualsNoCase(name.substr(name.size() - ext.size()), ext)) {
            return true;
        }
    }
    return false;
}

std::string RedactSecrets(std::string_view text, RedactionStats* stats) {
    if (text.empty()) return {};

    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        // ── Правило 1: блок приватного ключа ──────────────────────────────
        //
        // Найважливіше правило: приватний ключ не має потрапити нікуди
        // за жодних обставин.
        if (text[i] == '-' && StartsWith(text.substr(i), "-----BEGIN")) {
            const size_t lineEnd = text.find('\n', i);
            const std::string_view header =
                text.substr(i, (lineEnd == std::string_view::npos ? text.size() : lineEnd) - i);

            bool isPrivate = false;
            for (size_t k = 0; k + 11 <= header.size(); ++k) {
                if (EqualsNoCase(header.substr(k, 11), "PRIVATE KEY")) { isPrivate = true; break; }
            }

            if (isPrivate) {
                out += kMask;
                if (stats) stats->privateKeys += 1;
                // Пропускаємо весь блок до кінцевого маркера, якщо він є.
                const size_t endMarker = text.find("-----END", i);
                if (endMarker == std::string_view::npos) {
                    i = text.size();
                } else {
                    const size_t afterEnd = text.find('\n', endMarker);
                    i = (afterEnd == std::string_view::npos) ? text.size() : afterEnd;
                }
                continue;
            }
        }

        // ── Правило 2: заголовок авторизації ──────────────────────────────
        if ((text[i] == 'A' || text[i] == 'a') && i + 14 <= text.size() &&
            EqualsNoCase(text.substr(i, 14), "authorization:")) {
            out += text.substr(i, 14);
            i += 14;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) out += text[i++];

            const size_t valueStart = i;
            while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i;
            if (i > valueStart) {
                out += kMask;
                if (stats) stats->authHeaders += 1;
            }
            continue;
        }

        // ── Правило 3: облікові дані в URL ────────────────────────────────
        //
        // Формат схема://користувач:пароль@хост однозначний.
        if (text[i] == ':' && i + 2 < text.size() && text[i + 1] == '/' && text[i + 2] == '/') {
            const size_t authStart = i + 3;
            size_t scan = authStart;
            size_t colon = std::string_view::npos;

            while (scan < text.size() && text[scan] != '@' && text[scan] != '/' &&
                   text[scan] != ' ' && text[scan] != '\n') {
                if (text[scan] == ':' && colon == std::string_view::npos) colon = scan;
                ++scan;
            }

            if (scan < text.size() && text[scan] == '@' && colon != std::string_view::npos) {
                out += text.substr(i, 3);
                out += text.substr(authStart, colon - authStart);  // ім'я лишаємо
                out += ':';
                out += kMask;
                if (stats) stats->urlCredentials += 1;
                i = scan;  // символ '@' обробиться на наступній ітерації
                continue;
            }
        }

        // ── Правило 4: токени з відомим префіксом ─────────────────────────
        {
            bool matched = false;
            for (const KnownPrefix& known : kKnownPrefixes) {
                if (i + known.prefix.size() > text.size()) continue;
                if (text.compare(i, known.prefix.size(), known.prefix) != 0) continue;

                size_t end = i + known.prefix.size();
                while (end < text.size() && IsSecretValueChar(text[end])) ++end;

                // Довжина має відповідати формату: інакше це просто слово,
                // що випадково починається так само.
                if (end - i >= known.minLength) {
                    out += kMask;
                    if (stats) stats->knownPrefixes += 1;
                    i = end;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        // ── Правило 5: присвоєння секретній змінній ───────────────────────
        //
        // Спрацьовує лише тоді, коли ліворуч від роздільника стоїть саме
        // назва зі списку. Просте "="  чи ":" без такої назви не чіпаємо.
        if (text[i] == '=' || text[i] == ':') {
            const std::string_view name = ReadNameBefore(text, i);
            if (!name.empty() && IsSecretName(name)) {
                out += text[i];
                ++i;
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) out += text[i++];

                // Значення в лапках маскуємо разом із лапками.
                if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                    const char quote = text[i];
                    const size_t closing = text.find(quote, i + 1);
                    out += quote;
                    out += kMask;
                    if (closing == std::string_view::npos) {
                        i = text.size();
                    } else {
                        out += quote;
                        i = closing + 1;
                    }
                    if (stats) stats->assignments += 1;
                    continue;
                }

                const size_t valueStart = i;
                while (i < text.size() && IsSecretValueChar(text[i])) ++i;
                if (i > valueStart) {
                    out += kMask;
                    if (stats) stats->assignments += 1;
                }
                continue;
            }
        }

        out += text[i];
        ++i;
    }

    return out;
}

}  // namespace cm
