// crypto.cpp — реалізація криптографії через Windows CNG та DPAPI.

#include "crypto.h"

#include <bcrypt.h>
#include <dpapi.h>
#include <ncrypt.h>

#include <algorithm>
#include <cstring>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace cm::crypto {

namespace {

/// Обгортка над дескриптором алгоритму CNG.
class AlgHandle {
public:
    AlgHandle() = default;
    ~AlgHandle() { close(); }

    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    bool open(LPCWSTR algorithmId, DWORD flags = 0) {
        close();
        return NT_SUCCESS(::BCryptOpenAlgorithmProvider(&handle_, algorithmId, nullptr, flags));
    }

    void close() {
        if (handle_) { ::BCryptCloseAlgorithmProvider(handle_, 0); handle_ = nullptr; }
    }

    BCRYPT_ALG_HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

/// Обгортка над дескриптором хеша CNG.
class HashHandle {
public:
    ~HashHandle() { close(); }
    void close() { if (handle_) { ::BCryptDestroyHash(handle_); handle_ = nullptr; } }
    BCRYPT_HASH_HANDLE* addr() { return &handle_; }
    BCRYPT_HASH_HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

LPCWSTR AlgorithmFor(KeyKind kind) {
    return kind == KeyKind::Signing ? BCRYPT_ECDSA_P256_ALGORITHM
                                    : BCRYPT_ECDH_P256_ALGORITHM;
}

ULONG MagicFor(KeyKind kind, bool isPrivate) {
    if (kind == KeyKind::Signing) {
        return isPrivate ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    }
    return isPrivate ? BCRYPT_ECDH_PRIVATE_P256_MAGIC : BCRYPT_ECDH_PUBLIC_P256_MAGIC;
}

LPCWSTR BlobTypeFor(KeyKind kind, bool isPrivate) {
    if (kind == KeyKind::Signing) {
        return isPrivate ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB;
    }
    return isPrivate ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB;
}

}  // namespace

void SecureZero(void* data, size_t length) {
    // SecureZeroMemory гарантує, що оптимізатор не викине цей запис —
    // на відміну від звичайного memset.
    ::SecureZeroMemory(data, length);
}

// ── Випадкові числа ──────────────────────────────────────────────────────────

bool RandomBytes(uint8_t* out, size_t length) {
    if (length == 0) return true;
    if (length > 0xFFFFFFFF) return false;

    return NT_SUCCESS(::BCryptGenRandom(nullptr, out, static_cast<ULONG>(length),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool RandomBytes(std::vector<uint8_t>& out, size_t length) {
    out.resize(length);
    return RandomBytes(out.data(), length);
}

bool RandomBelow(uint32_t bound, uint32_t& out) {
    if (bound == 0) return false;

    // Відкидання зайвих значень прибирає зсув розподілу, який дало б
    // просте взяття залишку від ділення.
    const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % bound);

    for (int attempt = 0; attempt < 64; ++attempt) {
        uint32_t value = 0;
        if (!RandomBytes(reinterpret_cast<uint8_t*>(&value), sizeof(value))) return false;
        if (value < limit) { out = value % bound; return true; }
    }
    return false;
}

// ── Хеш і HMAC ───────────────────────────────────────────────────────────────

bool Sha256(const uint8_t* data, size_t length, Sha256Digest& out) {
    AlgHandle algorithm;
    if (!algorithm.open(BCRYPT_SHA256_ALGORITHM)) return false;

    HashHandle hash;
    if (!NT_SUCCESS(::BCryptCreateHash(algorithm.get(), hash.addr(), nullptr, 0,
                                       nullptr, 0, 0))) {
        return false;
    }
    if (length > 0 &&
        !NT_SUCCESS(::BCryptHashData(hash.get(), const_cast<PUCHAR>(data),
                                     static_cast<ULONG>(length), 0))) {
        return false;
    }
    return NT_SUCCESS(::BCryptFinishHash(hash.get(), out.data(),
                                         static_cast<ULONG>(out.size()), 0));
}

bool Sha256(std::string_view text, Sha256Digest& out) {
    return Sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(), out);
}

bool HmacSha256(const uint8_t* key, size_t keyLength,
                const uint8_t* data, size_t dataLength,
                Sha256Digest& out) {
    AlgHandle algorithm;
    if (!algorithm.open(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;

    HashHandle hash;
    if (!NT_SUCCESS(::BCryptCreateHash(algorithm.get(), hash.addr(), nullptr, 0,
                                       const_cast<PUCHAR>(key), static_cast<ULONG>(keyLength), 0))) {
        return false;
    }
    if (dataLength > 0 &&
        !NT_SUCCESS(::BCryptHashData(hash.get(), const_cast<PUCHAR>(data),
                                     static_cast<ULONG>(dataLength), 0))) {
        return false;
    }
    return NT_SUCCESS(::BCryptFinishHash(hash.get(), out.data(),
                                         static_cast<ULONG>(out.size()), 0));
}

bool HkdfSha256(const uint8_t* ikm, size_t ikmLength,
                const uint8_t* salt, size_t saltLength,
                std::string_view info,
                uint8_t* out, size_t outLength) {
    // RFC 5869 обмежує довжину виводу 255 блоками хеша.
    if (outLength == 0 || outLength > 255 * kSha256Size) return false;

    // Крок 1: extract — стискаємо вхідний матеріал у псевдовипадковий ключ.
    Sha256Digest prk{};
    const uint8_t zeroSalt[kSha256Size]{};
    const uint8_t* effectiveSalt = (salt && saltLength > 0) ? salt : zeroSalt;
    const size_t effectiveSaltLength = (salt && saltLength > 0) ? saltLength : kSha256Size;

    if (!HmacSha256(effectiveSalt, effectiveSaltLength, ikm, ikmLength, prk)) return false;

    // Крок 2: expand — розгортаємо PRK у потрібну кількість байтів.
    std::vector<uint8_t> block;
    std::vector<uint8_t> input;
    size_t produced = 0;
    uint8_t counter = 1;

    while (produced < outLength) {
        input.clear();
        input.insert(input.end(), block.begin(), block.end());
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);

        Sha256Digest digest{};
        if (!HmacSha256(prk.data(), prk.size(), input.data(), input.size(), digest)) {
            SecureZero(prk.data(), prk.size());
            return false;
        }

        block.assign(digest.begin(), digest.end());

        const size_t chunk = (outLength - produced < kSha256Size) ? (outLength - produced)
                                                                  : kSha256Size;
        std::memcpy(out + produced, digest.data(), chunk);
        produced += chunk;
        ++counter;
    }

    SecureZero(prk.data(), prk.size());
    if (!block.empty()) SecureZero(block.data(), block.size());
    return true;
}

// ── KeyPair ──────────────────────────────────────────────────────────────────

KeyPair::~KeyPair() { destroy(); }

KeyPair::KeyPair(KeyPair&& other) noexcept
    : handle_(other.handle_), key_(other.key_), kind_(other.kind_) {
    other.handle_ = nullptr;
    other.key_ = nullptr;
}

KeyPair& KeyPair::operator=(KeyPair&& other) noexcept {
    if (this != &other) {
        destroy();
        handle_ = other.handle_;
        key_ = other.key_;
        kind_ = other.kind_;
        other.handle_ = nullptr;
        other.key_ = nullptr;
    }
    return *this;
}

void KeyPair::destroy() {
    if (key_) {
        ::BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = nullptr;
    }
    if (handle_) {
        ::BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(handle_), 0);
        handle_ = nullptr;
    }
}

bool KeyPair::Generate(KeyKind kind) {
    destroy();
    kind_ = kind;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!NT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, AlgorithmFor(kind), nullptr, 0))) {
        return false;
    }
    handle_ = algorithm;

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!NT_SUCCESS(::BCryptGenerateKeyPair(algorithm, &key, 256, 0))) { destroy(); return false; }
    if (!NT_SUCCESS(::BCryptFinalizeKeyPair(key, 0))) {
        ::BCryptDestroyKey(key);
        destroy();
        return false;
    }

    key_ = key;
    return true;
}

bool KeyPair::ExportPrivate(std::vector<uint8_t>& blob) const {
    if (!key_) return false;

    ULONG needed = 0;
    if (!NT_SUCCESS(::BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr,
                                      BlobTypeFor(kind_, true), nullptr, 0, &needed, 0))) {
        return false;
    }

    blob.resize(needed);
    ULONG written = 0;
    if (!NT_SUCCESS(::BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr,
                                      BlobTypeFor(kind_, true), blob.data(), needed,
                                      &written, 0))) {
        blob.clear();
        return false;
    }
    blob.resize(written);
    return true;
}

bool KeyPair::Import(KeyKind kind, const std::vector<uint8_t>& blob) {
    destroy();
    kind_ = kind;
    if (blob.empty()) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!NT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, AlgorithmFor(kind), nullptr, 0))) {
        return false;
    }
    handle_ = algorithm;

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!NT_SUCCESS(::BCryptImportKeyPair(algorithm, nullptr, BlobTypeFor(kind, true), &key,
                                          const_cast<PUCHAR>(blob.data()),
                                          static_cast<ULONG>(blob.size()), 0))) {
        destroy();
        return false;
    }

    key_ = key;
    return true;
}

bool KeyPair::PublicPoint(std::array<uint8_t, kEcPointSize>& out) const {
    if (!key_) return false;

    ULONG needed = 0;
    if (!NT_SUCCESS(::BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr,
                                      BlobTypeFor(kind_, false), nullptr, 0, &needed, 0))) {
        return false;
    }

    std::vector<uint8_t> blob(needed);
    ULONG written = 0;
    if (!NT_SUCCESS(::BCryptExportKey(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr,
                                      BlobTypeFor(kind_, false), blob.data(), needed,
                                      &written, 0))) {
        return false;
    }

    // Формат блоба CNG: BCRYPT_ECCKEY_BLOB { Magic, cbKey } || X || Y.
    // Протокол очікує неспресовану точку 0x04 || X || Y — саме такий
    // формат розуміють і Android Keystore, і Node.js.
    if (written < sizeof(BCRYPT_ECCKEY_BLOB) + 64) return false;

    const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
    if (header->dwMagic != MagicFor(kind_, false)) return false;
    if (header->cbKey != 32) return false;

    out[0] = 0x04;
    std::memcpy(out.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    return true;
}

std::string KeyPair::PublicPointB64Url() const {
    std::array<uint8_t, kEcPointSize> point{};
    if (!PublicPoint(point)) return {};
    return Base64UrlEncode(point.data(), point.size());
}

bool KeyPair::Sign(std::string_view message, std::array<uint8_t, kSignatureSize>& out) const {
    if (!key_ || kind_ != KeyKind::Signing) return false;

    Sha256Digest digest{};
    if (!Sha256(message, digest)) return false;

    ULONG written = 0;
    // BCryptSignHash для ECDSA повертає підпис як r||s фіксованої довжини —
    // це і є формат IEEE P1363, узгоджений протоколом.
    if (!NT_SUCCESS(::BCryptSignHash(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr,
                                     digest.data(), static_cast<ULONG>(digest.size()),
                                     out.data(), static_cast<ULONG>(out.size()),
                                     &written, 0))) {
        return false;
    }
    return written == kSignatureSize;
}

bool KeyPair::DeriveShared(const uint8_t* peerPoint, size_t peerPointLength,
                           std::vector<uint8_t>& sharedSecret) const {
    if (!key_ || kind_ != KeyKind::Exchange) return false;
    if (peerPointLength != kEcPointSize || peerPoint[0] != 0x04) return false;

    // Складаємо блоб публічного ключа у форматі CNG з отриманої точки.
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = MagicFor(KeyKind::Exchange, false);
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), peerPoint + 1, 64);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!NT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDH_P256_ALGORITHM,
                                                  nullptr, 0))) {
        return false;
    }

    BCRYPT_KEY_HANDLE peerKey = nullptr;
    NTSTATUS status = ::BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey,
                                            blob.data(), static_cast<ULONG>(blob.size()), 0);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!NT_SUCCESS(status)) return false;

    BCRYPT_SECRET_HANDLE secret = nullptr;
    status = ::BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), peerKey, &secret, 0);
    ::BCryptDestroyKey(peerKey);
    if (!NT_SUCCESS(status)) return false;

    // Беремо сирий спільний секрет: розгортання ключа робить HKDF
    // на наступному кроці, однаково на обох платформах.
    ULONG needed = 0;
    status = ::BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, nullptr, 0, &needed, 0);
    if (!NT_SUCCESS(status)) { ::BCryptDestroySecret(secret); return false; }

    sharedSecret.resize(needed);
    ULONG written = 0;
    status = ::BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr,
                               sharedSecret.data(), needed, &written, 0);
    ::BCryptDestroySecret(secret);
    if (!NT_SUCCESS(status)) { sharedSecret.clear(); return false; }

    sharedSecret.resize(written);

    // BCRYPT_KDF_RAW_SECRET повертає координату X у зворотному порядку
    // байтів. Інші платформи очікують прямий порядок, тому розвертаємо.
    std::reverse(sharedSecret.begin(), sharedSecret.end());
    return true;
}

bool VerifySignature(const uint8_t* publicPoint, size_t publicPointLength,
                     std::string_view message,
                     const uint8_t* signature, size_t signatureLength) {
    if (publicPointLength != kEcPointSize || publicPoint[0] != 0x04) return false;
    if (signatureLength != kSignatureSize) return false;

    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), publicPoint + 1, 64);

    AlgHandle algorithm;
    if (!algorithm.open(BCRYPT_ECDSA_P256_ALGORITHM)) return false;

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!NT_SUCCESS(::BCryptImportKeyPair(algorithm.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                          blob.data(), static_cast<ULONG>(blob.size()), 0))) {
        return false;
    }

    Sha256Digest digest{};
    if (!Sha256(message, digest)) { ::BCryptDestroyKey(key); return false; }

    const NTSTATUS status = ::BCryptVerifySignature(
        key, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
        const_cast<PUCHAR>(signature), static_cast<ULONG>(signatureLength), 0);

    ::BCryptDestroyKey(key);
    return NT_SUCCESS(status);
}

// ── AES-256-GCM ──────────────────────────────────────────────────────────────

namespace {

bool OpenGcm(AlgHandle& algorithm) {
    if (!algorithm.open(BCRYPT_AES_ALGORITHM)) return false;
    return NT_SUCCESS(::BCryptSetProperty(algorithm.get(), BCRYPT_CHAINING_MODE,
                                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(
                                              BCRYPT_CHAIN_MODE_GCM)),
                                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
}

}  // namespace

bool AesGcmEncrypt(const SessionKey& key,
                   const uint8_t* nonce, size_t nonceLength,
                   std::string_view plaintext,
                   std::vector<uint8_t>& ciphertextWithTag) {
    if (nonceLength != kGcmNonceSize) return false;

    AlgHandle algorithm;
    if (!OpenGcm(algorithm)) return false;

    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (!NT_SUCCESS(::BCryptGenerateSymmetricKey(algorithm.get(), &keyHandle, nullptr, 0,
                                                 const_cast<PUCHAR>(key.data()),
                                                 static_cast<ULONG>(key.size()), 0))) {
        return false;
    }

    uint8_t tag[kGcmTagSize]{};
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(nonceLength);
    info.pbTag = tag;
    info.cbTag = kGcmTagSize;

    ciphertextWithTag.resize(plaintext.size() + kGcmTagSize);
    ULONG written = 0;

    const NTSTATUS status = ::BCryptEncrypt(
        keyHandle,
        reinterpret_cast<PUCHAR>(const_cast<char*>(plaintext.data())),
        static_cast<ULONG>(plaintext.size()),
        &info, nullptr, 0,
        ciphertextWithTag.data(), static_cast<ULONG>(plaintext.size()),
        &written, 0);

    ::BCryptDestroyKey(keyHandle);
    if (!NT_SUCCESS(status)) { ciphertextWithTag.clear(); return false; }

    // Тег дописується в кінець — так само, як це роблять реалізації
    // на інших платформах.
    std::memcpy(ciphertextWithTag.data() + written, tag, kGcmTagSize);
    ciphertextWithTag.resize(written + kGcmTagSize);
    return true;
}

bool AesGcmDecrypt(const SessionKey& key,
                   const uint8_t* nonce, size_t nonceLength,
                   const uint8_t* ciphertextWithTag, size_t length,
                   std::string& plaintext) {
    if (nonceLength != kGcmNonceSize) return false;
    if (length < kGcmTagSize) return false;

    const size_t ciphertextLength = length - kGcmTagSize;

    AlgHandle algorithm;
    if (!OpenGcm(algorithm)) return false;

    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (!NT_SUCCESS(::BCryptGenerateSymmetricKey(algorithm.get(), &keyHandle, nullptr, 0,
                                                 const_cast<PUCHAR>(key.data()),
                                                 static_cast<ULONG>(key.size()), 0))) {
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info{};
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(nonceLength);
    info.pbTag = const_cast<PUCHAR>(ciphertextWithTag + ciphertextLength);
    info.cbTag = kGcmTagSize;

    plaintext.resize(ciphertextLength);
    ULONG written = 0;

    // Невідповідність тегу дає STATUS_AUTH_TAG_MISMATCH. Це не «майже успіх»,
    // а сигнал, що повідомлення підроблене — результат відкидається цілком.
    const NTSTATUS status = ::BCryptDecrypt(
        keyHandle,
        const_cast<PUCHAR>(ciphertextWithTag), static_cast<ULONG>(ciphertextLength),
        &info, nullptr, 0,
        reinterpret_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(ciphertextLength),
        &written, 0);

    ::BCryptDestroyKey(keyHandle);

    if (!NT_SUCCESS(status)) {
        SecureZero(plaintext.data(), plaintext.size());
        plaintext.clear();
        return false;
    }

    plaintext.resize(written);
    return true;
}

// ── NonceCounter ─────────────────────────────────────────────────────────────

namespace {

/// Найменший номер nonce, якого ще не видав жоден лічильник з'єднання
/// в цьому процесі. Спільний для всіх телефонів: це лише зсуває старт
/// наступних лічильників угору й нічого не ламає.
LONG64 g_sessionNonceFloor = 0;

void RaiseSessionNonceFloor(uint64_t value) {
    for (;;) {
        const LONG64 current = g_sessionNonceFloor;
        if (static_cast<uint64_t>(current) >= value) return;
        if (::InterlockedCompareExchange64(&g_sessionNonceFloor, static_cast<LONG64>(value),
                                           current) == current) {
            return;
        }
    }
}

/// Секунди Unix — без залежності від інших модулів.
uint64_t UnixSecondsNow() {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (value.QuadPart < kEpochDiff100ns) return 0;
    return (value.QuadPart - kEpochDiff100ns) / 10'000'000ULL;
}

}  // namespace

NonceCounter::NonceCounter(const char direction[4]) {
    std::memcpy(direction_, direction, 4);
}

NonceCounter NonceCounter::ForSession(const char direction[4]) {
    NonceCounter counter(direction);
    const uint64_t clock = UnixSecondsNow() << 16;
    const uint64_t floor = static_cast<uint64_t>(g_sessionNonceFloor);
    counter.counter_ = clock > floor ? clock : floor;
    counter.session_ = true;
    return counter;
}

bool NonceCounter::Next(uint8_t out[kGcmNonceSize]) {
    if (exhausted()) return false;

    std::memcpy(out, direction_, 4);

    // Лічильник у мережевому порядку байтів — щоб обидві платформи
    // формували однакові значення.
    const uint64_t value = counter_++;
    if (session_) RaiseSessionNonceFloor(counter_);
    for (int i = 0; i < 8; ++i) {
        out[4 + i] = static_cast<uint8_t>((value >> ((7 - i) * 8)) & 0xFF);
    }
    return true;
}

// ── DPAPI ────────────────────────────────────────────────────────────────────

bool ProtectData(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& protectedData) {
    if (plaintext.empty()) return false;

    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    const_cast<BYTE*>(plaintext.data())};
    DATA_BLOB output{};

    // CRYPTPROTECT_UI_FORBIDDEN обов'язковий: Bridge працює у фоні,
    // і будь-яке модальне вікно там означало б зависання.
    if (!::CryptProtectData(&input, L"claude-monitor-bridge", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return false;
    }

    protectedData.assign(output.pbData, output.pbData + output.cbData);
    SecureZero(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return true;
}

bool UnprotectData(const std::vector<uint8_t>& protectedData, std::vector<uint8_t>& plaintext) {
    if (protectedData.empty()) return false;

    DATA_BLOB input{static_cast<DWORD>(protectedData.size()),
                    const_cast<BYTE*>(protectedData.data())};
    DATA_BLOB output{};

    if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return false;
    }

    plaintext.assign(output.pbData, output.pbData + output.cbData);
    SecureZero(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return true;
}

}  // namespace cm::crypto
