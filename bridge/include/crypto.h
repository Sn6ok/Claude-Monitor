// crypto.h — криптографія Bridge на Windows CNG.
//
// Жодного власного алгоритму: усе через системні примітиви
// (Частина 3 §13, §25 Master Prompt). CNG обрано тому, що він є частиною
// Windows — не треба ані постачати бібліотеку, ані оновлювати її окремо.
//
// Набір алгоритмів узгоджений із Android Keystore, щоб обидві сторони
// користувалися вбудованими засобами своїх платформ (docs/protocol.md §5):
//
//   підпис        ECDSA P-256 + SHA-256, формат IEEE P1363 (сирі r||s)
//   узгодження    ECDH P-256
//   похідні ключі HKDF-SHA256
//   шифрування    AES-256-GCM
//   зберігання    DPAPI, прив'язка до облікового запису користувача

#pragma once

#include "common.h"

#include <array>

namespace cm::crypto {

inline constexpr size_t kEcPointSize   = 65;  // 0x04 || X(32) || Y(32)
inline constexpr size_t kSignatureSize = 64;  // r(32) || s(32)
inline constexpr size_t kSha256Size    = 32;
inline constexpr size_t kAesKeySize    = 32;  // AES-256
inline constexpr size_t kGcmNonceSize  = 12;
inline constexpr size_t kGcmTagSize    = 16;

using Sha256Digest = std::array<uint8_t, kSha256Size>;
using SessionKey   = std::array<uint8_t, kAesKeySize>;

/// Заповнює буфер криптографічно стійкими випадковими байтами.
/// Джерело — системний ГВЧ; власної реалізації немає.
bool RandomBytes(uint8_t* out, size_t length);
bool RandomBytes(std::vector<uint8_t>& out, size_t length);

/// Ціле число в діапазоні [0, bound) без зсуву розподілу.
bool RandomBelow(uint32_t bound, uint32_t& out);

// ── Хеш і HMAC ───────────────────────────────────────────────────────────────

bool Sha256(const uint8_t* data, size_t length, Sha256Digest& out);
bool Sha256(std::string_view text, Sha256Digest& out);

bool HmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* data, size_t dataLength,
                Sha256Digest& out);

/// HKDF (RFC 5869) на основі HMAC-SHA256.
bool HkdfSha256(const uint8_t* ikm, size_t ikmLength,
                const uint8_t* salt, size_t saltLength,
                std::string_view info,
                uint8_t* out, size_t outLength);

// ── Ключова пара ─────────────────────────────────────────────────────────────

enum class KeyKind : uint8_t { Signing, Exchange };

/// Ключова пара P-256. Приватна частина ніколи не залишає процес
/// у відкритому вигляді: назовні вона віддається лише зашифрованою DPAPI.
class KeyPair {
public:
    KeyPair() = default;
    ~KeyPair();

    KeyPair(const KeyPair&) = delete;
    KeyPair& operator=(const KeyPair&) = delete;
    KeyPair(KeyPair&& other) noexcept;
    KeyPair& operator=(KeyPair&& other) noexcept;

    /// Створює нову пару ключів.
    bool Generate(KeyKind kind);

    /// Відновлює пару з раніше збереженого блоба.
    bool Import(KeyKind kind, const std::vector<uint8_t>& blob);

    /// Експортує приватну частину для збереження. Викликати лише
    /// безпосередньо перед захистом через DPAPI.
    bool ExportPrivate(std::vector<uint8_t>& blob) const;

    /// Публічний ключ як сира неспресована точка — саме в такому вигляді
    /// він передається в протоколі.
    bool PublicPoint(std::array<uint8_t, kEcPointSize>& out) const;
    std::string PublicPointB64Url() const;

    /// Підписує повідомлення. Формат підпису — IEEE P1363 (сирі r||s),
    /// саме такий CNG повертає нативно.
    bool Sign(std::string_view message, std::array<uint8_t, kSignatureSize>& out) const;

    /// Обчислює спільний секрет ECDH із публічною точкою іншої сторони.
    bool DeriveShared(const uint8_t* peerPoint, size_t peerPointLength,
                      std::vector<uint8_t>& sharedSecret) const;

    bool valid() const { return key_ != nullptr; }

private:
    void destroy();

    void*   handle_ = nullptr;  // BCRYPT_ALG_HANDLE
    void*   key_ = nullptr;     // BCRYPT_KEY_HANDLE
    KeyKind kind_ = KeyKind::Signing;
};

/// Перевіряє підпис чужим публічним ключем.
bool VerifySignature(const uint8_t* publicPoint, size_t publicPointLength,
                     std::string_view message,
                     const uint8_t* signature, size_t signatureLength);

// ── AES-256-GCM ──────────────────────────────────────────────────────────────

/// Шифрує повідомлення. Результат: шифротекст із доданим 16-байтним тегом.
bool AesGcmEncrypt(const SessionKey& key,
                   const uint8_t* nonce, size_t nonceLength,
                   std::string_view plaintext,
                   std::vector<uint8_t>& ciphertextWithTag);

/// Дешифрує повідомлення й перевіряє тег автентичності.
/// @returns false як за помилки, так і за невідповідності тегу — тобто
///          за будь-якої спроби підміни вмісту.
bool AesGcmDecrypt(const SessionKey& key,
                   const uint8_t* nonce, size_t nonceLength,
                   const uint8_t* ciphertextWithTag, size_t length,
                   std::string& plaintext);

/// Лічильник nonce для AES-GCM.
///
/// Повторне використання nonce з тим самим ключем — найнебезпечніша помилка
/// в GCM: вона розкриває відкритий текст. Тому лічильник монотонний,
/// а при досягненні межі клас відмовляє замість того, щоб піти на друге коло.
class NonceCounter {
public:
    /// @param direction 4 байти напрямку: "B2M " або "M2B "
    explicit NonceCounter(const char direction[4]);

    /// Лічильник для нового з'єднання.
    ///
    /// Ключ шифрування між з'єднаннями не змінюється, тож нумерація не може
    /// щоразу починатися з нуля: саме так раніше й було, і після кожного
    /// перепідключення nonce повторювались. Тепер старт — секунди Unix,
    /// зсунуті на 16 біт (до 65 536 повідомлень на кожну секунду часу),
    /// і не нижче за будь-який номер, уже виданий у цьому процесі.
    static NonceCounter ForSession(const char direction[4]);

    /// @returns false, коли простір лічильника вичерпано
    bool Next(uint8_t out[kGcmNonceSize]);

    uint64_t count() const { return counter_; }
    bool exhausted() const { return counter_ >= kMaxCounter; }

private:
    // Межа навмисно значно нижча за технічну: 2^48 повідомлень
    // недосяжні на практиці, а запас лишається великим.
    static constexpr uint64_t kMaxCounter = 1ULL << 48;

    char     direction_[4]{};
    uint64_t counter_ = 0;
    /// Номери цього лічильника піднімають спільну нижню межу процесу.
    bool     session_ = false;
};

// ── Захищене зберігання ──────────────────────────────────────────────────────

/// Захищає дані через DPAPI. Розшифрувати їх зможе лише той самий
/// обліковий запис Windows на цій самій машині.
bool ProtectData(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& protectedData);
bool UnprotectData(const std::vector<uint8_t>& protectedData, std::vector<uint8_t>& plaintext);

/// Затирає буфер так, щоб компілятор не міг прибрати цей запис
/// як «непотрібний».
void SecureZero(void* data, size_t length);

}  // namespace cm::crypto
