// test_phones.cpp — кілька спарених телефонів і лічильник nonce з'єднання.

#include "test_framework.h"
#include "config.h"
#include "crypto.h"
#include "wire.h"

#include <string>
#include <vector>

using namespace cm;

namespace {

PairedPhone MakePhone(char fill, uint64_t pairedAtMs = 1000) {
    PairedPhone phone;
    phone.deviceId = std::string(64, fill);
    phone.publicSign = std::string("sig-") + fill;
    phone.publicExchange = std::string("ecdh-") + fill;
    phone.pairedAtMs = pairedAtMs;
    return phone;
}

}  // namespace

CM_TEST(phones, json_round_trip_keeps_order) {
    const std::vector<PairedPhone> phones = {MakePhone('a', 1), MakePhone('b', 2), MakePhone('c', 3)};
    const std::vector<PairedPhone> parsed = ParsePhonesJson(BuildPhonesJson(phones));

    CHECK_EQ(parsed.size(), 3u);
    if (parsed.size() == 3) {
        CHECK_STR(parsed[0].deviceId, phones[0].deviceId);
        CHECK_STR(parsed[2].publicExchange, "ecdh-c");
        CHECK(parsed[1].pairedAtMs == 2u);
    }
}

CM_TEST(phones, reads_old_single_phone_file) {
    // Так peer.json виглядав, коли телефон міг бути лише один.
    const std::string old = R"({"device_id":")" + std::string(64, 'd') +
                            R"(","pub_sig":"S","pub_ecdh":"E"})";
    const std::vector<PairedPhone> parsed = ParsePhonesJson(old);

    CHECK_EQ(parsed.size(), 1u);
    if (!parsed.empty()) {
        CHECK_STR(parsed[0].publicExchange, "E");
        CHECK(parsed[0].pairedAtMs == 0u);
    }
}

CM_TEST(phones, empty_or_broken_file_means_no_phones) {
    CHECK(ParsePhonesJson("").empty());
    CHECK(ParsePhonesJson("{").empty());
    // Файл після --unpair у давньому форматі: поля є, але порожні.
    CHECK(ParsePhonesJson(R"({"device_id":"","pub_sig":"","pub_ecdh":""})").empty());
}

CM_TEST(phones, file_is_capped_and_deduplicated) {
    std::vector<PairedPhone> many;
    for (char c : std::string("abcdefg")) many.push_back(MakePhone(c));
    many.insert(many.begin() + 1, MakePhone('a'));

    const std::vector<PairedPhone> parsed = ParsePhonesJson(BuildPhonesJson(many));
    CHECK_EQ(parsed.size(), kMaxPairedPhones);
    if (parsed.size() >= 2) {
        CHECK_STR(parsed[0].deviceId, std::string(64, 'a'));
        CHECK_STR(parsed[1].deviceId, std::string(64, 'b'));
    }
}

CM_TEST(phones, add_phone_respects_limit_and_updates_known) {
    Identity identity;
    for (char c : std::string("abcde")) CHECK(identity.AddPhone(MakePhone(c)));

    // Шостий — ні: спершу треба від'єднати один із наявних.
    CHECK_FALSE(identity.AddPhone(MakePhone('f')));
    CHECK_EQ(identity.phones.size(), 5u);

    // Повторне підключення вже відомого телефона місця не займає.
    CHECK(identity.AddPhone(MakePhone('c', 999)));
    CHECK_EQ(identity.phones.size(), 5u);

    const PairedPhone* found = identity.FindPhone(std::string(64, 'c'));
    CHECK(found != nullptr);
    if (found) CHECK(found->pairedAtMs == 999u);
    CHECK(identity.FindPhone(std::string(64, 'z')) == nullptr);
}

CM_TEST(phones, remove_by_prefix) {
    Identity identity;
    identity.AddPhone(MakePhone('a'));
    identity.AddPhone(MakePhone('b'));

    CHECK_EQ(identity.RemovePhones("aaaa"), 1u);
    CHECK_EQ(identity.phones.size(), 1u);
    CHECK_EQ(identity.RemovePhones(""), 0u);
    CHECK_EQ(identity.RemovePhones("zzzz"), 0u);
    CHECK(identity.paired());
}

CM_TEST(phones, session_nonce_counters_never_overlap) {
    // Ключ між з'єднаннями сталий, тож номери нового з'єднання мають бути
    // більшими за всі номери попереднього. Раніше обидва починали з нуля.
    crypto::NonceCounter first = crypto::NonceCounter::ForSession("B2M ");
    uint8_t nonce[crypto::kGcmNonceSize]{};
    for (int i = 0; i < 100; ++i) CHECK(first.Next(nonce));
    const uint64_t lastOfFirst = first.count() - 1;

    crypto::NonceCounter second = crypto::NonceCounter::ForSession("B2M ");
    CHECK(second.count() > lastOfFirst);

    // Старт прив'язаний до часу, а не до нуля.
    CHECK(second.count() >= ((NowUnixMs() / 1000) - 5) << 16);
    CHECK_FALSE(second.exhausted());

    // Звичайний лічильник, як і раніше, починає з нуля.
    CHECK(crypto::NonceCounter("B2M ").count() == 0u);
}

CM_TEST(phones, forward_frame_names_recipient) {
    const std::string recipient(64, 'e');
    const std::string addressed = MakeForwardFrame("AAAA", "BBBB", recipient);
    CHECK(addressed.find(R"("to":")" + recipient) != std::string::npos);

    // Без адресата поля немає зовсім.
    CHECK(MakeForwardFrame("AAAA", "BBBB").find(R"("to")") == std::string::npos);
}
