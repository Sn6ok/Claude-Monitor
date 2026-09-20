// pairing.h — первинне встановлення довіри між ноутбуком і телефоном.
//
// Схема з docs/protocol.md §4. Ключова властивість: Relay бере участь
// у передачі, але НЕ може підмінити ключі, бо не знає коду pairing.
// Володіння кодом підтверджується через HMAC над публічними ключами.
//
// Код показується користувачу на ноутбуці й вводиться на телефоні.
// Він одноразовий, живе 180 секунд і ніколи не записується на диск
// (Частина 3 §7 Master Prompt).

#pragma once

#include "common.h"
#include "crypto.h"

#include <string>

namespace cm {

/// Алфавіт коду: Base32 без символів, які легко сплутати
/// (I, L, O, U виключені). 32 символи -> 5 бітів на знак.
inline constexpr char kPairingAlphabet[] = "ABCDEFGHJKMNPQRSTVWXYZ0123456789";
inline constexpr size_t kPairingCodeLength = 12;   // 12 * 5 = 60 бітів
inline constexpr uint32_t kPairingTtlSec = 180;

/// Стан процедури pairing на боці Bridge.
class PairingSession {
public:
    /// Створює новий код і супутні секрети.
    bool Begin(const crypto::KeyPair& signing, const crypto::KeyPair& exchange);

    /// Код у зручному для читання вигляді: XXXX-XXXX-XXXX
    std::string formattedCode() const;

    /// Ідентифікатор пропозиції для Relay (16 випадкових байтів у hex).
    const std::string& offerId() const { return offerId_; }

    /// Код підтвердження, який Bridge надсилає разом із пропозицією.
    const std::string& bridgeConfirm() const { return bridgeConfirm_; }

    const std::string& publicSign() const { return publicSign_; }
    const std::string& publicExchange() const { return publicExchange_; }

    /// Перевіряє підтвердження від телефона.
    ///
    /// Саме тут відсікається підміна ключів: обчислити правильний HMAC
    /// може лише той, хто знає код, а код бачив тільки користувач.
    bool VerifyPeer(std::string_view peerPublicSign,
                    std::string_view peerPublicExchange,
                    std::string_view peerConfirm) const;

    bool expired(uint64_t nowMs) const;
    bool active() const { return !offerId_.empty(); }

    /// Стирає код і похідні секрети з пам'яті.
    void Clear();

private:
    bool DeriveKey(crypto::SessionKey& out) const;

    std::string code_;            ///< лише в пам'яті, ніколи на диску
    std::string offerId_;
    std::string publicSign_;
    std::string publicExchange_;
    std::string bridgeConfirm_;
    uint64_t    createdAtMs_ = 0;
};

/// Обчислює код підтвердження для однієї зі сторін.
///
/// @param role "bridge" або "monitor" — розділення ролей не дає
///        перевикористати підтвердження однієї сторони для іншої.
std::string ComputeConfirm(std::string_view code,
                           std::string_view role,
                           std::string_view publicSign,
                           std::string_view publicExchange);

/// Нормалізує введений користувачем код: прибирає дефіси й пробіли,
/// переводить у верхній регістр, виправляє типові плутанини символів.
std::string NormalizePairingCode(std::string_view input);

}  // namespace cm
