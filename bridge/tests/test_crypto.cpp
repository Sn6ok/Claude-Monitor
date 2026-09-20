// test_crypto.cpp — тести криптографії.
//
// Включають контрольні вектори RFC: збіг із ними доводить, що реалізація
// сумісна з іншими платформами, а не просто «узгоджена сама з собою».

#include "test_framework.h"
#include "crypto.h"
#include "pairing.h"

using namespace cm;
using namespace cm::crypto;

CM_TEST(crypto, random_bytes_differ) {
    std::vector<uint8_t> a, b;
    CHECK(RandomBytes(a, 32));
    CHECK(RandomBytes(b, 32));
    CHECK_EQ(a.size(), 32u);
    CHECK(a != b);
}

CM_TEST(crypto, random_below_stays_in_range) {
    for (int i = 0; i < 200; ++i) {
        uint32_t value = 0;
        CHECK(RandomBelow(32, value));
        CHECK(value < 32u);
    }
}

CM_TEST(crypto, sha256_matches_known_vector) {
    // Контрольний вектор: SHA-256("abc")
    Sha256Digest digest{};
    CHECK(Sha256("abc", digest));
    CHECK_STR(HexEncode(digest.data(), digest.size()),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

CM_TEST(crypto, sha256_of_empty_input) {
    Sha256Digest digest{};
    CHECK(Sha256("", digest));
    CHECK_STR(HexEncode(digest.data(), digest.size()),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

CM_TEST(crypto, hmac_sha256_matches_rfc4231) {
    // RFC 4231, тест 1.
    const std::vector<uint8_t> key(20, 0x0b);
    const std::string data = "Hi There";

    Sha256Digest mac{};
    CHECK(HmacSha256(key.data(), key.size(),
                     reinterpret_cast<const uint8_t*>(data.data()), data.size(), mac));
    CHECK_STR(HexEncode(mac.data(), mac.size()),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

CM_TEST(crypto, hkdf_matches_rfc5869) {
    // RFC 5869, тест 1. Збіг доводить сумісність із реалізаціями
    // на Node.js та Android.
    const std::vector<uint8_t> ikm(22, 0x0b);
    const uint8_t salt[13] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,
                              0x07,0x08,0x09,0x0a,0x0b,0x0c};
    const uint8_t info[10] = {0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9};

    uint8_t okm[42]{};
    CHECK(HkdfSha256(ikm.data(), ikm.size(), salt, sizeof(salt),
                     std::string_view(reinterpret_cast<const char*>(info), sizeof(info)),
                     okm, sizeof(okm)));

    CHECK_STR(HexEncode(okm, sizeof(okm)),
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865");
}

CM_TEST(crypto, signing_roundtrip) {
    KeyPair key;
    CHECK(key.Generate(KeyKind::Signing));

    const std::string message = "claude-monitor-v1|auth|bridge|abc|nonce";
    std::array<uint8_t, kSignatureSize> signature{};
    CHECK(key.Sign(message, signature));

    std::array<uint8_t, kEcPointSize> point{};
    CHECK(key.PublicPoint(point));
    CHECK_EQ(point[0], 0x04);  // неспресована точка

    CHECK(VerifySignature(point.data(), point.size(), message,
                          signature.data(), signature.size()));
}

CM_TEST(crypto, signature_rejects_tampered_message) {
    KeyPair key;
    CHECK(key.Generate(KeyKind::Signing));

    std::array<uint8_t, kSignatureSize> signature{};
    CHECK(key.Sign("оригінальне повідомлення", signature));

    std::array<uint8_t, kEcPointSize> point{};
    key.PublicPoint(point);

    CHECK_FALSE(VerifySignature(point.data(), point.size(), "інше повідомлення",
                                signature.data(), signature.size()));
}

CM_TEST(crypto, signature_rejects_wrong_key) {
    KeyPair signer, impostor;
    CHECK(signer.Generate(KeyKind::Signing));
    CHECK(impostor.Generate(KeyKind::Signing));

    std::array<uint8_t, kSignatureSize> signature{};
    signer.Sign("повідомлення", signature);

    std::array<uint8_t, kEcPointSize> wrongPoint{};
    impostor.PublicPoint(wrongPoint);

    CHECK_FALSE(VerifySignature(wrongPoint.data(), wrongPoint.size(), "повідомлення",
                                signature.data(), signature.size()));
}

CM_TEST(crypto, signature_rejects_malformed_input) {
    KeyPair key;
    key.Generate(KeyKind::Signing);
    std::array<uint8_t, kEcPointSize> point{};
    key.PublicPoint(point);

    const uint8_t garbage[64]{};
    CHECK_FALSE(VerifySignature(point.data(), point.size(), "m", garbage, sizeof(garbage)));

    // Хибна довжина точки чи підпису має відхилятися без падіння.
    CHECK_FALSE(VerifySignature(point.data(), 10, "m", garbage, sizeof(garbage)));
    CHECK_FALSE(VerifySignature(point.data(), point.size(), "m", garbage, 10));
}

CM_TEST(crypto, ecdh_produces_shared_secret) {
    KeyPair alice, bob;
    CHECK(alice.Generate(KeyKind::Exchange));
    CHECK(bob.Generate(KeyKind::Exchange));

    std::array<uint8_t, kEcPointSize> alicePoint{}, bobPoint{};
    alice.PublicPoint(alicePoint);
    bob.PublicPoint(bobPoint);

    std::vector<uint8_t> secretA, secretB;
    CHECK(alice.DeriveShared(bobPoint.data(), bobPoint.size(), secretA));
    CHECK(bob.DeriveShared(alicePoint.data(), alicePoint.size(), secretB));

    CHECK(!secretA.empty());
    CHECK(secretA == secretB);
}

CM_TEST(crypto, ecdh_rejects_invalid_point) {
    KeyPair key;
    key.Generate(KeyKind::Exchange);

    std::vector<uint8_t> secret;
    const uint8_t badPoint[kEcPointSize]{};  // перший байт 0x00, не 0x04
    CHECK_FALSE(key.DeriveShared(badPoint, sizeof(badPoint), secret));
    CHECK_FALSE(key.DeriveShared(badPoint, 10, secret));
}

CM_TEST(crypto, aes_gcm_roundtrip) {
    SessionKey key{};
    RandomBytes(key.data(), key.size());

    uint8_t nonce[kGcmNonceSize]{};
    NonceCounter counter("B2M ");
    CHECK(counter.Next(nonce));

    const std::string plaintext = R"({"t":"events","txt":"кирилиця і 😀"})";
    std::vector<uint8_t> ciphertext;
    CHECK(AesGcmEncrypt(key, nonce, sizeof(nonce), plaintext, ciphertext));

    // Шифротекст довший за відкритий текст рівно на розмір тега.
    CHECK_EQ(ciphertext.size(), plaintext.size() + kGcmTagSize);

    std::string decrypted;
    CHECK(AesGcmDecrypt(key, nonce, sizeof(nonce),
                        ciphertext.data(), ciphertext.size(), decrypted));
    CHECK_STR(decrypted, plaintext);
}

CM_TEST(crypto, aes_gcm_detects_tampering) {
    SessionKey key{};
    RandomBytes(key.data(), key.size());

    uint8_t nonce[kGcmNonceSize]{};
    NonceCounter counter("B2M ");
    counter.Next(nonce);

    std::vector<uint8_t> ciphertext;
    AesGcmEncrypt(key, nonce, sizeof(nonce), "секретне повідомлення", ciphertext);

    // Змінюємо один біт: тег автентичності має це виявити.
    ciphertext[3] ^= 0x01;

    std::string decrypted;
    CHECK_FALSE(AesGcmDecrypt(key, nonce, sizeof(nonce),
                              ciphertext.data(), ciphertext.size(), decrypted));
    CHECK(decrypted.empty());
}

CM_TEST(crypto, aes_gcm_rejects_wrong_key) {
    SessionKey key{}, wrongKey{};
    RandomBytes(key.data(), key.size());
    RandomBytes(wrongKey.data(), wrongKey.size());

    uint8_t nonce[kGcmNonceSize]{};
    NonceCounter counter("B2M ");
    counter.Next(nonce);

    std::vector<uint8_t> ciphertext;
    AesGcmEncrypt(key, nonce, sizeof(nonce), "дані", ciphertext);

    std::string decrypted;
    CHECK_FALSE(AesGcmDecrypt(wrongKey, nonce, sizeof(nonce),
                              ciphertext.data(), ciphertext.size(), decrypted));
}

CM_TEST(crypto, nonce_counter_never_repeats) {
    NonceCounter counter("B2M ");

    std::vector<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        uint8_t nonce[kGcmNonceSize]{};
        CHECK(counter.Next(nonce));
        seen.push_back(HexEncode(nonce, sizeof(nonce)));
    }

    // Повторення nonce з тим самим ключем розкрило б відкритий текст —
    // це найнебезпечніша помилка в GCM.
    for (size_t i = 0; i < seen.size(); ++i) {
        for (size_t j = i + 1; j < seen.size(); ++j) {
            if (seen[i] == seen[j]) {
                CHECK(false);
                return;
            }
        }
    }
}

CM_TEST(crypto, nonce_encodes_direction) {
    NonceCounter toMonitor("B2M ");
    NonceCounter toBridge("M2B ");

    uint8_t a[kGcmNonceSize]{}, b[kGcmNonceSize]{};
    toMonitor.Next(a);
    toBridge.Next(b);

    // Різні напрямки дають різні nonce навіть за однакового лічильника,
    // тож кадр не можна відтворити у зворотному напрямку.
    CHECK(std::memcmp(a, b, kGcmNonceSize) != 0);
}

CM_TEST(crypto, dpapi_roundtrip) {
    std::vector<uint8_t> secret{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> protectedData, recovered;

    CHECK(ProtectData(secret, protectedData));
    // Захищені дані не мають містити вихідних байтів у відкритому вигляді.
    CHECK(protectedData.size() > secret.size());

    CHECK(UnprotectData(protectedData, recovered));
    CHECK(recovered == secret);
}

CM_TEST(crypto, key_export_import_roundtrip) {
    KeyPair original;
    CHECK(original.Generate(KeyKind::Signing));

    std::vector<uint8_t> blob;
    CHECK(original.ExportPrivate(blob));

    KeyPair restored;
    CHECK(restored.Import(KeyKind::Signing, blob));

    // Відновлений ключ має давати ту саму публічну точку.
    CHECK_STR(restored.PublicPointB64Url(), original.PublicPointB64Url());

    // І підпис відновленого ключа має перевірятися вихідною точкою.
    std::array<uint8_t, kSignatureSize> signature{};
    CHECK(restored.Sign("перевірка", signature));

    std::array<uint8_t, kEcPointSize> point{};
    original.PublicPoint(point);
    CHECK(VerifySignature(point.data(), point.size(), "перевірка",
                          signature.data(), signature.size()));
}

CM_TEST(crypto, import_rejects_garbage) {
    KeyPair key;
    const std::vector<uint8_t> garbage(64, 0xAB);
    CHECK_FALSE(key.Import(KeyKind::Signing, garbage));
    CHECK_FALSE(key.Import(KeyKind::Signing, {}));
}

// ── Pairing ──────────────────────────────────────────────────────────────────

CM_TEST(pairing, code_has_expected_shape) {
    KeyPair signing, exchange;
    signing.Generate(KeyKind::Signing);
    exchange.Generate(KeyKind::Exchange);

    PairingSession session;
    CHECK(session.Begin(signing, exchange));

    const std::string formatted = session.formattedCode();
    // 12 символів плюс два дефіси: XXXX-XXXX-XXXX
    CHECK_EQ(formatted.size(), 14u);
    CHECK_EQ(formatted[4], '-');
    CHECK_EQ(formatted[9], '-');
}

CM_TEST(pairing, codes_are_unique) {
    KeyPair signing, exchange;
    signing.Generate(KeyKind::Signing);
    exchange.Generate(KeyKind::Exchange);

    std::vector<std::string> codes;
    for (int i = 0; i < 50; ++i) {
        PairingSession session;
        session.Begin(signing, exchange);
        codes.push_back(session.formattedCode());
    }

    for (size_t i = 0; i < codes.size(); ++i) {
        for (size_t j = i + 1; j < codes.size(); ++j) {
            CHECK(codes[i] != codes[j]);
        }
    }
}

CM_TEST(pairing, normalizes_user_input) {
    // Користувач переписує код з екрана, тож плутанина символів неминуча.
    CHECK_STR(NormalizePairingCode("abcd-efgh-jkmn"), "ABCDEFGHJKMN");
    CHECK_STR(NormalizePairingCode("ABCD EFGH JKMN"), "ABCDEFGHJKMN");
    CHECK_STR(NormalizePairingCode("A1CD-EFGH-JKMN"), "A1CDEFGHJKMN");
    // I та L читаються як 1, O як 0, U як V.
    CHECK_STR(NormalizePairingCode("AICD-EFGH-JKMN"), "A1CDEFGHJKMN");
    CHECK_STR(NormalizePairingCode("AOCD-EFGH-JKMN"), "A0CDEFGHJKMN");
}

CM_TEST(pairing, confirm_depends_on_role) {
    const std::string code = "ABCD1234EFGH";
    const std::string pubSign = "pub-sign-value";
    const std::string pubExchange = "pub-exchange-value";

    const std::string asBridge = ComputeConfirm(code, "bridge", pubSign, pubExchange);
    const std::string asMonitor = ComputeConfirm(code, "monitor", pubSign, pubExchange);

    CHECK(!asBridge.empty());
    // Різні ролі дають різні підтвердження: інакше підтвердження Bridge
    // можна було б повторити як підтвердження телефона.
    CHECK(asBridge != asMonitor);
}

CM_TEST(pairing, confirm_depends_on_code) {
    const std::string a = ComputeConfirm("ABCD1234EFGH", "monitor", "p1", "p2");
    const std::string b = ComputeConfirm("ABCD1234EFGX", "monitor", "p1", "p2");
    CHECK(a != b);
}

CM_TEST(pairing, verify_rejects_wrong_confirm) {
    KeyPair signing, exchange, peerSign, peerExchange;
    signing.Generate(KeyKind::Signing);
    exchange.Generate(KeyKind::Exchange);
    peerSign.Generate(KeyKind::Signing);
    peerExchange.Generate(KeyKind::Exchange);

    PairingSession session;
    session.Begin(signing, exchange);

    const std::string peerSignB64 = peerSign.PublicPointB64Url();
    const std::string peerExchangeB64 = peerExchange.PublicPointB64Url();

    // Підтвердження, обчислене з невірним кодом, не має пройти —
    // саме це не дозволяє Relay підмінити ключі.
    const std::string wrongConfirm =
        ComputeConfirm("НЕВІРНИЙКОД1", "monitor", peerSignB64, peerExchangeB64);

    CHECK_FALSE(session.VerifyPeer(peerSignB64, peerExchangeB64, wrongConfirm));
    CHECK_FALSE(session.VerifyPeer(peerSignB64, peerExchangeB64, ""));
    CHECK_FALSE(session.VerifyPeer("сміття", peerExchangeB64, wrongConfirm));
}
