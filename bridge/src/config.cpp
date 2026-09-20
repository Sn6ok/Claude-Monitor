// config.cpp — конфігурація та зберігання ідентичності.

#include "config.h"
#include "json.h"

#include <algorithm>

namespace cm {

namespace {

/// Формат файлу ідентичності:
///   [4 байти] версія
///   [4 байти] довжина блоба ключа підпису
///   [ ...   ] блоб ключа підпису
///   [4 байти] довжина блоба ключа обміну
///   [ ...   ] блоб ключа обміну
/// Увесь цей вміст шифрується DPAPI перед записом на диск.
constexpr uint32_t kIdentityFormatVersion = 1;

void AppendUint32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool ReadUint32(const std::vector<uint8_t>& data, size_t& offset, uint32_t& value) {
    if (offset + 4 > data.size()) return false;
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool ReadBlob(const std::vector<uint8_t>& data, size_t& offset, std::vector<uint8_t>& blob) {
    uint32_t length = 0;
    if (!ReadUint32(data, offset, length)) return false;
    // Верхня межа рятує від спроби виділити гігабайти через пошкоджений файл.
    if (length > 64 * 1024 || offset + length > data.size()) return false;
    blob.assign(data.begin() + static_cast<ptrdiff_t>(offset),
                data.begin() + static_cast<ptrdiff_t>(offset + length));
    offset += length;
    return true;
}

}  // namespace

// ── Config ───────────────────────────────────────────────────────────────────

bool Config::Load() {
    if (dataDir.empty()) dataDir = BridgeDataDir();
    if (dataDir.empty()) return false;
    if (logPath.empty()) logPath = dataDir + L"\\bridge.log";

    std::string content;
    if (!ReadWholeFile(configPath(), content, 64 * 1024)) return false;

    json::Value root;
    if (!json::Parse(content, root) || !root.isObject()) {
        Log().Warn("конфігурацію пошкоджено, використовую значення за замовчуванням");
        return false;
    }

    const std::string url = root["relay_url"].asStringOr("");
    if (!url.empty()) relayUrl = Utf8ToUtf16(url);

    const std::string level = root["log_level"].asStringOr("");
    if (!level.empty()) logLevel = ParseLogLevel(level);

    allowInsecure = root["allow_insecure"].asBool(false);
    return true;
}

bool Config::Save() const {
    if (!EnsureDirectory(dataDir)) return false;

    std::string out;
    json::Writer writer(out);
    writer.beginObject();
    writer.field("relay_url", Utf16ToUtf8(relayUrl));
    writer.field("log_level", ToString(logLevel));
    writer.field("allow_insecure", allowInsecure);
    writer.endObject();

    return WriteWholeFileAtomic(configPath(), out);
}

// ── Identity ─────────────────────────────────────────────────────────────────

bool Identity::Create(const Config& config) {
    if (!signing.Generate(crypto::KeyKind::Signing)) {
        Log().Error("не вдалося створити ключ підпису");
        return false;
    }
    if (!exchange.Generate(crypto::KeyKind::Exchange)) {
        Log().Error("не вдалося створити ключ обміну");
        return false;
    }

    std::array<uint8_t, crypto::kEcPointSize> point{};
    if (!signing.PublicPoint(point)) return false;

    crypto::Sha256Digest digest{};
    if (!crypto::Sha256(point.data(), point.size(), digest)) return false;
    deviceId = HexEncode(digest.data(), digest.size());

    if (!Save(config)) {
        Log().Error("не вдалося зберегти ідентичність");
        return false;
    }

    Log().Infof("створено нову ідентичність пристрою: %s…", deviceId.substr(0, 8).c_str());
    return true;
}

bool Identity::Save(const Config& config) const {
    if (!EnsureDirectory(config.dataDir)) return false;

    std::vector<uint8_t> signingBlob, exchangeBlob;
    if (!signing.ExportPrivate(signingBlob)) return false;
    if (!exchange.ExportPrivate(exchangeBlob)) return false;

    std::vector<uint8_t> plain;
    AppendUint32(plain, kIdentityFormatVersion);
    AppendUint32(plain, static_cast<uint32_t>(signingBlob.size()));
    plain.insert(plain.end(), signingBlob.begin(), signingBlob.end());
    AppendUint32(plain, static_cast<uint32_t>(exchangeBlob.size()));
    plain.insert(plain.end(), exchangeBlob.begin(), exchangeBlob.end());

    // Приватні ключі не залишають процес у відкритому вигляді: одразу
    // після експорту вони шифруються, а буфери затираються.
    std::vector<uint8_t> encrypted;
    const bool protectedOk = crypto::ProtectData(plain, encrypted);

    crypto::SecureZero(plain.data(), plain.size());
    crypto::SecureZero(signingBlob.data(), signingBlob.size());
    crypto::SecureZero(exchangeBlob.data(), exchangeBlob.size());

    if (!protectedOk) {
        Log().Error("DPAPI не змогла захистити ідентичність");
        return false;
    }

    const std::string_view bytes(reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
    if (!WriteWholeFileAtomic(config.identityPath(), bytes)) return false;

    return SavePhones(config);
}

bool Identity::Load(const Config& config) {
    std::string raw;
    if (!ReadWholeFile(config.identityPath(), raw, 256 * 1024)) return false;
    if (raw.empty()) return false;

    const std::vector<uint8_t> encrypted(raw.begin(), raw.end());
    std::vector<uint8_t> plain;
    if (!crypto::UnprotectData(encrypted, plain)) {
        // Найімовірніша причина — файл скопійовано з іншої машини або
        // з-під іншого облікового запису. Це не помилка формату, а саме
        // те, від чого DPAPI і захищає.
        Log().Warn("не вдалося розшифрувати ідентичність (інший обліковий запис або машина)");
        return false;
    }

    size_t offset = 0;
    uint32_t version = 0;
    if (!ReadUint32(plain, offset, version) || version != kIdentityFormatVersion) {
        crypto::SecureZero(plain.data(), plain.size());
        return false;
    }

    std::vector<uint8_t> signingBlob, exchangeBlob;
    const bool parsed = ReadBlob(plain, offset, signingBlob) &&
                        ReadBlob(plain, offset, exchangeBlob);

    crypto::SecureZero(plain.data(), plain.size());
    if (!parsed) return false;

    const bool imported = signing.Import(crypto::KeyKind::Signing, signingBlob) &&
                          exchange.Import(crypto::KeyKind::Exchange, exchangeBlob);

    crypto::SecureZero(signingBlob.data(), signingBlob.size());
    crypto::SecureZero(exchangeBlob.data(), exchangeBlob.size());
    if (!imported) return false;

    std::array<uint8_t, crypto::kEcPointSize> point{};
    if (!signing.PublicPoint(point)) return false;

    crypto::Sha256Digest digest{};
    if (!crypto::Sha256(point.data(), point.size(), digest)) return false;
    deviceId = HexEncode(digest.data(), digest.size());

    // Дані телефонів — публічні, тож лежать окремим відкритим файлом.
    LoadPhones(config);
    return true;
}

bool Identity::LoadPhones(const Config& config) {
    std::string raw;
    if (!ReadWholeFile(config.peerPath(), raw, 64 * 1024)) {
        // Файлу немає — телефонів не підключено. Якщо ж файл є, але не
        // прочитався, перелік не чіпаємо: інакше випадкова невдача читання
        // від'єднала б усі телефони.
        if (::GetFileAttributesW(config.peerPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
            phones.clear();
            return true;
        }
        return false;
    }

    phones = ParsePhonesJson(raw);
    return true;
}

bool Identity::SavePhones(const Config& config) const {
    if (!EnsureDirectory(config.dataDir)) return false;
    return WriteWholeFileAtomic(config.peerPath(), BuildPhonesJson(phones));
}

const PairedPhone* Identity::FindPhone(std::string_view phoneId) const {
    for (const PairedPhone& phone : phones) {
        if (phone.deviceId == phoneId) return &phone;
    }
    return nullptr;
}

bool Identity::AddPhone(const PairedPhone& phone) {
    for (PairedPhone& known : phones) {
        if (known.deviceId != phone.deviceId) continue;
        // Той самий телефон підключили ще раз — оновлюємо дані, місця не займаємо.
        known = phone;
        return true;
    }
    if (phones.size() >= kMaxPairedPhones) return false;
    phones.push_back(phone);
    return true;
}

size_t Identity::RemovePhones(std::string_view prefix) {
    if (prefix.empty()) return 0;

    const size_t before = phones.size();
    phones.erase(std::remove_if(phones.begin(), phones.end(),
                                [prefix](const PairedPhone& phone) {
                                    return std::string_view(phone.deviceId).substr(0, prefix.size()) ==
                                           prefix;
                                }),
                 phones.end());
    return before - phones.size();
}

bool Identity::DeriveSessionKey(const PairedPhone& phone, crypto::SessionKey& out) const {
    if (phone.publicExchange.empty() || phone.deviceId.empty()) return false;

    std::vector<uint8_t> peerPoint;
    if (!Base64UrlDecode(phone.publicExchange, peerPoint)) return false;
    if (peerPoint.size() != crypto::kEcPointSize) return false;

    std::vector<uint8_t> shared;
    if (!exchange.DeriveShared(peerPoint.data(), peerPoint.size(), shared)) return false;

    // Сіль — обидва ідентифікатори пристроїв у сталому порядку. Це гарантує,
    // що Bridge і телефон отримають однаковий ключ незалежно від того,
    // хто з них рахує (docs/protocol.md §5). Кожен телефон має власний
    // ідентифікатор, а отже й власний ключ.
    std::string salt = deviceId < phone.deviceId ? deviceId + phone.deviceId
                                                 : phone.deviceId + deviceId;

    const bool derived = crypto::HkdfSha256(
        shared.data(), shared.size(),
        reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
        "claude-monitor-v1/e2ee",
        out.data(), out.size());

    crypto::SecureZero(shared.data(), shared.size());
    return derived;
}

// ── Файл телефонів ───────────────────────────────────────────────────────────

std::vector<PairedPhone> ParsePhonesJson(std::string_view text) {
    std::vector<PairedPhone> phones;

    json::Value root;
    if (text.empty() || !json::Parse(text, root) || !root.isObject()) return phones;

    const auto readPhone = [](const json::Value& item) {
        PairedPhone phone;
        phone.deviceId = item["device_id"].asStringOr("");
        phone.publicSign = item["pub_sig"].asStringOr("");
        phone.publicExchange = item["pub_ecdh"].asStringOr("");
        const int64_t pairedAt = item["paired_at"].asInt(0);
        phone.pairedAtMs = pairedAt > 0 ? static_cast<uint64_t>(pairedAt) : 0;
        return phone;
    };

    // Порожні записи й повтори відкидаються, телефонів — не більше межі:
    // файл міг бути змінений вручну, і довіряти йому наосліп не можна.
    const auto accept = [&phones](PairedPhone phone) {
        if (phone.deviceId.empty() || phone.publicExchange.empty()) return;
        for (const PairedPhone& known : phones) {
            if (known.deviceId == phone.deviceId) return;
        }
        if (phones.size() < kMaxPairedPhones) phones.push_back(std::move(phone));
    };

    const json::Value& list = root["phones"];
    if (list.isArray()) {
        for (const json::Value& item : list.asArray()) {
            if (item.isObject()) accept(readPhone(item));
        }
    } else {
        // Давній формат: єдиний телефон полями верхнього рівня.
        accept(readPhone(root));
    }
    return phones;
}

std::string BuildPhonesJson(const std::vector<PairedPhone>& phones) {
    std::string out;
    json::Writer writer(out);
    writer.beginObject();
    writer.key("phones");
    writer.beginArray();
    for (const PairedPhone& phone : phones) {
        writer.beginObject();
        writer.field("device_id", phone.deviceId);
        writer.field("pub_sig", phone.publicSign);
        writer.field("pub_ecdh", phone.publicExchange);
        writer.field("paired_at", phone.pairedAtMs);
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
    return out;
}

}  // namespace cm
