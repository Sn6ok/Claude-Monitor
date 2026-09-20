// config.h — конфігурація та стійке зберігання ідентичності Bridge.
//
// Секрети НІКОЛИ не лежать у конфігураційному файлі у відкритому вигляді
// (Частина 3 §6, Частина 5 §24 Master Prompt). Приватні ключі зберігаються
// окремим файлом, зашифрованим через DPAPI: розшифрувати їх може лише той
// самий обліковий запис Windows на цій самій машині.

#pragma once

#include "common.h"
#include "crypto.h"
#include "logging.h"

#include <string_view>
#include <vector>

namespace cm {

struct Config {
    /// Адреса Relay, наприклад wss://monitor.example.com/ws
    std::wstring relayUrl;

    LogLevel     logLevel = LogLevel::Info;
    bool         logToConsole = false;
    std::wstring logPath;

    /// Каталог даних Bridge (%USERPROFILE%\.claude-monitor).
    std::wstring dataDir;

    /// Дозволити незахищене з'єднання (ws://) — лише для локальної розробки.
    /// У зібраному релізі прапорець ігнорується: небезпечний запасний
    /// варіант заборонено (Частина 3 §23 Master Prompt).
    bool allowInsecure = false;

    bool Load();
    bool Save() const;

    std::wstring configPath() const { return dataDir + L"\\bridge.json"; }
    std::wstring identityPath() const { return dataDir + L"\\identity.bin"; }
    /// Перелік спарених телефонів. Назва файлу лишилася від часів, коли телефон
    /// міг бути лише один: так старі встановлення підхоплюються без міграції.
    std::wstring peerPath() const { return dataDir + L"\\peer.json"; }
};

/// Скільки телефонів можна одночасно підключити до одного ноутбука.
inline constexpr size_t kMaxPairedPhones = 5;

/// Спарений телефон — лише публічні дані, тож зберігається відкритим файлом.
struct PairedPhone {
    std::string deviceId;        ///< SHA-256 від публічної точки підпису телефона, hex
    std::string publicSign;      ///< публічна точка підпису, base64url
    std::string publicExchange;  ///< публічна точка ECDH, base64url
    uint64_t    pairedAtMs = 0;  ///< коли підключено (мс Unix); 0 — невідомо
};

/// Розбирає файл телефонів. Розуміє і давній формат з одним телефоном.
/// Порожні записи й повтори відкидаються, телефонів — не більше kMaxPairedPhones.
std::vector<PairedPhone> ParsePhonesJson(std::string_view json);

/// Формує файл телефонів.
std::string BuildPhonesJson(const std::vector<PairedPhone>& phones);

/// Криптографічна ідентичність цієї машини та спарені телефони.
struct Identity {
    crypto::KeyPair signing;    ///< ECDSA P-256 — автентифікація на Relay
    crypto::KeyPair exchange;   ///< ECDH P-256 — наскрізне шифрування

    std::string deviceId;       ///< SHA-256 від публічної точки підпису, hex

    /// Спарені телефони в порядку підключення. Кожен має власний ключ
    /// шифрування: те, що надсилається одному телефону, інший не прочитає.
    std::vector<PairedPhone> phones;

    bool paired() const { return !phones.empty(); }

    /// Телефон за точним ідентифікатором. nullptr — такого немає.
    const PairedPhone* FindPhone(std::string_view phoneId) const;

    /// Створює нову пару ключів і зберігає її під захистом DPAPI.
    bool Create(const Config& config);

    /// Завантажує ідентичність із диска. false, якщо її ще немає.
    bool Load(const Config& config);

    /// Зберігає ідентичність. Приватна частина шифрується DPAPI.
    bool Save(const Config& config) const;

    /// Перечитує лише перелік телефонів: його змінює окремий процес --pair.
    bool LoadPhones(const Config& config);

    /// Записує перелік телефонів.
    bool SavePhones(const Config& config) const;

    /// Додає телефон або оновлює ключі вже відомого.
    /// @returns false, якщо вже підключено kMaxPairedPhones інших телефонів
    bool AddPhone(const PairedPhone& phone);

    /// Прибирає телефони, чий ідентифікатор починається з `prefix`.
    /// @returns скільки телефонів прибрано
    size_t RemovePhones(std::string_view prefix);

    /// Забуває всі телефони — наприклад, коли відкликано сам Bridge.
    void ForgetAllPhones() { phones.clear(); }

    /// Обчислює спільний ключ наскрізного шифрування з конкретним телефоном.
    bool DeriveSessionKey(const PairedPhone& phone, crypto::SessionKey& out) const;
};

}  // namespace cm
