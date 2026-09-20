// pairing.cpp — реалізація первинного встановлення довіри.

#include "pairing.h"
#include "logging.h"

#include <cstring>

namespace cm {

namespace {

/// Матеріал для HKDF, з якого виводиться ключ підтвердження.
constexpr char kPairingSalt[] = "claude-monitor-v1/pairing";

}  // namespace

std::string NormalizePairingCode(std::string_view input) {
    std::string out;
    out.reserve(kPairingCodeLength);

    for (char c : input) {
        if (c == '-' || c == ' ' || c == '\t') continue;

        // Верхній регістр для ASCII.
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');

        // Символи, виключені з алфавіту, найчастіше вводять помилково
        // замість схожих цифр. Виправляємо мовчки — користувач переписує
        // код з екрана, і така плутанина неминуча.
        switch (c) {
            case 'I': case 'L': c = '1'; break;
            case 'O':           c = '0'; break;
            case 'U':           c = 'V'; break;
            default: break;
        }

        if (std::strchr(kPairingAlphabet, c) == nullptr) continue;
        out += c;

        if (out.size() >= kPairingCodeLength) break;
    }
    return out;
}

std::string ComputeConfirm(std::string_view code,
                           std::string_view role,
                           std::string_view publicSign,
                           std::string_view publicExchange) {
    // Ключ підтвердження виводиться з коду через HKDF, а не береться
    // безпосередньо: так довжина й розподіл ключа не залежать від того,
    // яким саме вийшов код.
    uint8_t pairingKey[32]{};
    if (!crypto::HkdfSha256(reinterpret_cast<const uint8_t*>(code.data()), code.size(),
                            reinterpret_cast<const uint8_t*>(kPairingSalt),
                            sizeof(kPairingSalt) - 1,
                            "", pairingKey, sizeof(pairingKey))) {
        return {};
    }

    // У повідомлення входить роль: інакше підтвердження, надіслане
    // Bridge, можна було б повторити як підтвердження телефона.
    std::string message;
    message.reserve(role.size() + publicSign.size() + publicExchange.size());
    message.append(role);
    message.append(publicSign);
    message.append(publicExchange);

    crypto::Sha256Digest mac{};
    const bool ok = crypto::HmacSha256(
        pairingKey, sizeof(pairingKey),
        reinterpret_cast<const uint8_t*>(message.data()), message.size(), mac);

    crypto::SecureZero(pairingKey, sizeof(pairingKey));
    if (!ok) return {};

    return Base64UrlEncode(mac.data(), mac.size());
}

// ── PairingSession ───────────────────────────────────────────────────────────

bool PairingSession::Begin(const crypto::KeyPair& signing, const crypto::KeyPair& exchange) {
    Clear();

    // Код формується з криптографічно стійкого джерела, рівномірно
    // по алфавіту. 12 символів по 5 бітів дають 60 бітів ентропії —
    // підбір за 180 секунд недосяжний (docs/protocol.md §4.2).
    code_.reserve(kPairingCodeLength);
    for (size_t i = 0; i < kPairingCodeLength; ++i) {
        uint32_t index = 0;
        if (!crypto::RandomBelow(32, index)) {
            Log().Error("немає доступу до джерела випадковості");
            Clear();
            return false;
        }
        code_ += kPairingAlphabet[index];
    }

    std::vector<uint8_t> offerBytes;
    if (!crypto::RandomBytes(offerBytes, 16)) { Clear(); return false; }
    offerId_ = HexEncode(offerBytes.data(), offerBytes.size());

    publicSign_ = signing.PublicPointB64Url();
    publicExchange_ = exchange.PublicPointB64Url();
    if (publicSign_.empty() || publicExchange_.empty()) { Clear(); return false; }

    bridgeConfirm_ = ComputeConfirm(code_, "bridge", publicSign_, publicExchange_);
    if (bridgeConfirm_.empty()) { Clear(); return false; }

    createdAtMs_ = NowMonotonicMs();

    // Код НЕ потрапляє в журнал за жодного рівня деталізації
    // (Частина 3 §24, Частина 4 §31 Master Prompt).
    Log().Infof("розпочато pairing, пропозиція %s…, діє %u с",
                offerId_.substr(0, 8).c_str(), kPairingTtlSec);
    return true;
}

std::string PairingSession::formattedCode() const {
    if (code_.size() != kPairingCodeLength) return code_;

    // Групування по чотири полегшує переписування з екрана.
    std::string out;
    out.reserve(kPairingCodeLength + 2);
    for (size_t i = 0; i < code_.size(); ++i) {
        if (i > 0 && i % 4 == 0) out += '-';
        out += code_[i];
    }
    return out;
}

bool PairingSession::VerifyPeer(std::string_view peerPublicSign,
                                std::string_view peerPublicExchange,
                                std::string_view peerConfirm) const {
    if (code_.empty()) return false;
    if (peerConfirm.empty()) return false;

    // Публічні точки мають бути коректними ще до перевірки HMAC:
    // так відсікається зіпсований або підроблений вхід.
    std::vector<uint8_t> signPoint, exchangePoint;
    if (!Base64UrlDecode(peerPublicSign, signPoint)) return false;
    if (!Base64UrlDecode(peerPublicExchange, exchangePoint)) return false;
    if (signPoint.size() != crypto::kEcPointSize || signPoint[0] != 0x04) return false;
    if (exchangePoint.size() != crypto::kEcPointSize || exchangePoint[0] != 0x04) return false;

    const std::string expected =
        ComputeConfirm(code_, "monitor", peerPublicSign, peerPublicExchange);
    if (expected.empty()) return false;
    if (expected.size() != peerConfirm.size()) return false;

    // Порівняння за сталий час: різниця в часі відповіді інакше видавала б
    // побайтово, наскільки близьким був підбір.
    unsigned char difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        difference |= static_cast<unsigned char>(expected[i] ^ peerConfirm[i]);
    }
    return difference == 0;
}

bool PairingSession::expired(uint64_t nowMs) const {
    if (createdAtMs_ == 0) return true;
    if (nowMs < createdAtMs_) return false;  // годинник зсунули назад
    return (nowMs - createdAtMs_) > static_cast<uint64_t>(kPairingTtlSec) * 1000;
}

void PairingSession::Clear() {
    // Код і підтвердження затираються, а не просто звільняються:
    // після завершення pairing вони не мають лишатися в пам'яті процесу.
    if (!code_.empty()) crypto::SecureZero(code_.data(), code_.size());
    if (!bridgeConfirm_.empty()) crypto::SecureZero(bridgeConfirm_.data(), bridgeConfirm_.size());

    code_.clear();
    offerId_.clear();
    publicSign_.clear();
    publicExchange_.clear();
    bridgeConfirm_.clear();
    createdAtMs_ = 0;
}

}  // namespace cm
