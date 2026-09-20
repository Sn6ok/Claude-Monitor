// redact.h — маскування секретів у тексті, що йде на телефон.
//
// ВАЖЛИВО про роль цього модуля.
//
// Маскування — це ДОДАТКОВИЙ шар безпеки, а не основний механізм
// (Частина 5 §23 Master Prompt). Основний захист полягає в тому, що Bridge
// узагалі не передає вміст файлів, аргументи інструментів чи повний stdout —
// лише короткі нормалізовані описи дій.
//
// Master Prompt (Частина 3 §18) прямо застерігає від «небезпечного
// універсального regex-фільтра, який може ламати звичайний output».
// Тому правила тут навмисно вузькі: кожне спрацьовує лише за наявності
// явного контексту (назва змінної, префікс відомого формату, заголовок),
// а не за самим лише виглядом рядка.

#pragma once

#include <string>
#include <string_view>

namespace cm {

struct RedactionStats {
    uint32_t assignments = 0;   ///< PASSWORD=..., api_key: ...
    uint32_t knownPrefixes = 0; ///< sk-..., ghp_..., AKIA...
    uint32_t privateKeys = 0;   ///< -----BEGIN ... PRIVATE KEY-----
    uint32_t authHeaders = 0;   ///< Authorization: Bearer ...
    uint32_t urlCredentials = 0;///< https://user:pass@host

    uint32_t total() const {
        return assignments + knownPrefixes + privateKeys + authHeaders + urlCredentials;
    }
};

/// Маскує потенційні секрети. Повертає оброблений текст.
/// @param stats якщо не nullptr, накопичує статистику спрацювань
std::string RedactSecrets(std::string_view text, RedactionStats* stats = nullptr);

/// Чи виглядає шлях як файл, який зазвичай містить секрети.
/// Використовується, щоб не показувати навіть імені файлу в деталях події.
bool IsSensitivePath(std::string_view path);

}  // namespace cm
