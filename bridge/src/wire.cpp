// wire.cpp — формування та розбір кадрів протоколу.

#include "wire.h"
#include "logging.h"

#include <algorithm>

namespace cm {

namespace {

constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// Допустимий розбіг годинників — має збігатися з Relay.
constexpr uint64_t kClockSkewMs = 300'000;

/// Типи кадрів, які Bridge готовий прийняти. Усе інше відхиляється:
/// мовчазна обробка невідомого типу — це шлях до розбіжності реалізацій
/// (Частина 4 §23 Master Prompt).
bool IsKnownIncomingType(std::string_view type) {
    return type == "challenge" || type == "auth_ok" || type == "fwd" ||
           type == "error" || type == "ping" || type == "pong" ||
           type == "pair_claim" || type == "pair_ok";
}

}  // namespace

std::string MakeUlid() {
    // Перші 10 символів — час, решта 16 — випадковість. Криптографічної
    // стійкості тут не потрібно: це лише ідентифікатор кадру для
    // співвіднесення з підтвердженням.
    std::string out;
    out.reserve(26);

    uint64_t time = NowUnixMs();
    char timePart[10];
    for (int i = 9; i >= 0; --i) {
        timePart[i] = kCrockford[time % 32];
        time /= 32;
    }
    out.append(timePart, 10);

    uint8_t random[10]{};
    if (!crypto::RandomBytes(random, sizeof(random))) {
        // Запасний варіант на випадок відмови джерела випадковості:
        // унікальності в межах процесу достатньо.
        static uint64_t counter = 0;
        const uint64_t value = ++counter;
        for (size_t i = 0; i < sizeof(random); ++i) {
            random[i] = static_cast<uint8_t>((value >> (i * 5)) & 0xFF);
        }
    }

    for (int i = 0; i < 16; ++i) {
        const int bitPos = i * 5;
        const int byteIdx = bitPos >> 3;
        const int shift = bitPos & 7;
        const int chunk = ((random[byteIdx] << 8) | (byteIdx + 1 < 10 ? random[byteIdx + 1] : 0))
                          >> (11 - shift);
        out += kCrockford[chunk & 31];
    }
    return out;
}

// ── Рівень 1 ─────────────────────────────────────────────────────────────────

std::string MakeFrame(std::string_view type, std::string_view payloadJson) {
    std::string out;
    json::Writer writer(out);

    writer.beginObject();
    writer.field("v", static_cast<int64_t>(kProtocolVersion));
    writer.field("t", type);
    writer.field("id", MakeUlid());
    writer.field("ts", NowUnixMs());
    writer.key("p");
    writer.raw(payloadJson.empty() ? "{}" : payloadJson);
    writer.endObject();

    return out;
}

std::string MakeHelloFrame(std::string_view deviceId) {
    std::string payload;
    json::Writer writer(payload);
    writer.beginObject();
    writer.field("role", "bridge");
    writer.field("device_id", deviceId);
    writer.field("client", std::string("claude-monitor-bridge/") + kBridgeVersionA);
    writer.endObject();

    return MakeFrame("hello", payload);
}

std::string MakeAuthFrame(std::string_view deviceId, std::string_view signatureB64) {
    std::string payload;
    json::Writer writer(payload);
    writer.beginObject();
    writer.field("device_id", deviceId);
    writer.field("sig", signatureB64);
    writer.endObject();

    return MakeFrame("auth", payload);
}

std::string MakePingFrame() {
    return MakeFrame("ping", "{}");
}

std::string MakePairOfferFrame(std::string_view offerId,
                               std::string_view publicSign,
                               std::string_view publicExchange,
                               std::string_view confirm) {
    std::string payload;
    json::Writer writer(payload);
    writer.beginObject();
    writer.field("offer_id", offerId);
    writer.field("pub_sig", publicSign);
    writer.field("pub_ecdh", publicExchange);
    writer.field("confirm", confirm);
    writer.endObject();

    return MakeFrame("pair_offer", payload);
}

std::string MakePairOkFrame(std::string_view offerId, bool accepted) {
    std::string payload;
    json::Writer writer(payload);
    writer.beginObject();
    writer.field("offer_id", offerId);
    writer.field("accepted", accepted);
    writer.endObject();

    return MakeFrame("pair_ok", payload);
}

std::string MakeForwardFrame(std::string_view nonceB64,
                             std::string_view ciphertextB64,
                             std::string_view to) {
    std::string payload;
    json::Writer writer(payload);
    writer.beginObject();
    writer.field("n", nonceB64);
    writer.field("ct", ciphertextB64);
    // Кожен телефон має власний ключ, тож кадр адресується саме йому:
    // іншим телефонам він був би лише шумом, який не розшифрувати.
    if (!to.empty()) writer.field("to", to);
    writer.endObject();

    return MakeFrame("fwd", payload);
}

IncomingFrame ParseIncomingFrame(std::string_view raw, uint64_t nowMs) {
    IncomingFrame frame;

    // Розмір перевіряється до розбору: кадр на кілька мегабайтів має бути
    // відхилений, не дійшовши до парсера.
    if (raw.empty() || raw.size() > kMaxFrameBytes) return frame;

    json::Value root;
    if (!json::Parse(raw, root) || !root.isObject()) return frame;

    if (root["v"].asInt(0) != kProtocolVersion) {
        Log().Warn("отримано кадр несумісної версії протоколу");
        return frame;
    }

    frame.type = root["t"].asStringOr("");
    if (frame.type.empty() || !IsKnownIncomingType(frame.type)) return frame;

    // Позначка часу поза допустимим вікном означає або збій годинника,
    // або спробу повторного відтворення старого кадру.
    const int64_t timestamp = root["ts"].asInt(0);
    if (timestamp <= 0) return frame;

    const uint64_t theirs = static_cast<uint64_t>(timestamp);
    const uint64_t difference = theirs > nowMs ? theirs - nowMs : nowMs - theirs;
    if (difference > kClockSkewMs) {
        Log().Warn("відхилено кадр із неприйнятною позначкою часу");
        return frame;
    }

    frame.payload = root["p"];
    frame.valid = true;
    return frame;
}

std::string AuthSigningString(std::string_view role,
                              std::string_view deviceId,
                              std::string_view nonceB64) {
    // Префікс із назвою та версією протоколу відокремлює контекст:
    // підпис для автентифікації не можна перевикористати деінде.
    std::string out;
    out.reserve(64 + deviceId.size() + nonceB64.size());
    out += "claude-monitor-v1|auth|";
    out.append(role);
    out += '|';
    out.append(deviceId);
    out += '|';
    out.append(nonceB64);
    return out;
}

// ── Рівень 2 ─────────────────────────────────────────────────────────────────

namespace {

/// Записує одну подію як об'єкт JSON.
///
/// `maxText` обмежує текст: у пакеті подій він іде повністю, а у знімку
/// скорочується, бо знімок — один кадр на всі задачі одразу.
void WriteEvent(json::Writer& writer, const Event& event, size_t maxText) {
    const auto writeText = [&](const char* name) {
        if (event.text.empty()) return;
        if (event.text.size() <= maxText) {
            writer.field(name, event.text);
        } else {
            writer.field(name, TruncateUtf8(event.text, maxText));
        }
    };

    writer.beginObject();
    // sid є в КОЖНІЙ події без винятку: саме це не дозволяє приписати
    // подію не тій задачі (docs/architecture.md §5.1).
    writer.field("sid", event.sessionId);
    writer.field("seq", event.sequence);
    writer.field("ts", event.timestampMs);
    writer.field("k", ToString(event.kind));

    switch (event.kind) {
        case EventKind::Status:
            writer.field("state", ToString(event.state));
            writeText("text");
            break;

        case EventKind::Activity:
            writer.field("action", ToString(event.action));
            if (!event.target.empty()) writer.field("target", event.target);
            break;

        case EventKind::Result:
            writer.field("status", event.isError ? "error" : "success");
            writeText("text");
            break;

        case EventKind::Session:
            writer.field("event", event.text);
            writer.field("state", ToString(event.state));
            break;

        case EventKind::Node:
            // Ім'я вузла їде поруч із текстом: у чаті має бути видно, що це
            // рядок мода, а не слова Claude.
            if (!event.target.empty()) writer.field("node", event.target);
            writeText("text");
            break;

        case EventKind::Output:
        case EventKind::Prompt:
            writeText("text");
            // Кількість зображень у запиті: самі зображення не передаються.
            if (event.imageCount > 0) {
                writer.field("img", static_cast<uint64_t>(event.imageCount));
            }
            break;
    }

    // Частина довгої репліки. Поля пишуться лише тоді, коли ділити довелося:
    // для звичайної події вони нічого не додають, крім байтів.
    if (event.partCount > 1) {
        writer.field("gid", event.groupId);
        writer.field("pt", static_cast<int64_t>(event.part));
        writer.field("pc", static_cast<int64_t>(event.partCount));
    }

    // Час відповіді пишеться лише тоді, коли він є: більшість подій його не несе.
    if (event.turnStartedAtMs != 0) writer.field("turn_start", event.turnStartedAtMs);
    if (event.turnMs != 0) writer.field("turn_ms", event.turnMs);

    writer.endObject();
}

void WriteSession(json::Writer& writer, const SessionState& session, size_t maxEvents) {
    writer.beginObject();
    writer.field("sid", session.sessionId);
    writer.field("short", session.sessionId.substr(0, 8));
    writer.field("title", session.title);
    // Джерело назви передається окремо, щоб застосунок міг чесно показати,
    // що назва похідна, а не задана людиною (docs/protocol.md §6.1).
    writer.field("title_src", ToString(session.titleSource));
    writer.field("project", session.project);
    // Повний шлях теж: коли відкрито кілька проєктів із схожими назвами
    // каталогів, самої назви замало, щоб їх розрізнити.
    if (!session.cwd.empty()) writer.field("cwd", session.cwd);
    if (!session.gitBranch.empty()) writer.field("branch", session.gitBranch);
    writer.field("state", ToString(session.state));

    // Розмір контексту розмови. Межу моделі приймач знає сам —
    // Bridge передає лише фактичне число, без вигаданих відсотків.
    if (session.contextTokens > 0) {
        writer.field("context_tokens", session.contextTokens);
        if (!session.model.empty()) writer.field("model", session.model);
    }

    if (session.hasActivity) {
        writer.key("activity");
        writer.beginObject();
        writer.field("action", ToString(session.currentAction));
        if (!session.currentTarget.empty()) writer.field("target", session.currentTarget);
        writer.endObject();
    }

    writer.field("started_at", session.startedAtMs);
    writer.field("last_update", session.lastUpdateMs);
    writer.field("seq", session.lastSequence);

    // Таймер відповіді: коли почалася поточна (лише якщо початок бачили)
    // і скільки тривала попередня.
    if (session.turnStartKnown && session.turnStartedAtMs != 0) {
        writer.field("turn_started_at", session.turnStartedAtMs);
    }
    if (session.lastTurnMs != 0) writer.field("last_turn_ms", session.lastTurnMs);

    writer.key("events");
    writer.beginArray();
    // Лише останні події: коли знімок не вміщується в кадр, їх стає менше.
    const size_t skip = session.recentEvents.size() > maxEvents
        ? session.recentEvents.size() - maxEvents
        : 0;
    for (size_t index = skip; index < session.recentEvents.size(); ++index) {
        WriteEvent(writer, session.recentEvents[index], kMaxSnapshotText);
    }
    writer.endArray();

    writer.endObject();
}

}  // namespace

std::string BuildSnapshotJson(const SessionManager& manager,
                              std::string_view bridgeHost,
                              uint64_t uptimeSec,
                              bool claudeDesktopRunning,
                              double cpuPercent,
                              double rssMb,
                              size_t maxEventsPerSession,
                              const std::vector<NodeResult>* nodes) {
    std::string out;
    json::Writer writer(out);

    writer.beginObject();
    writer.field("t", "snapshot");
    writer.field("seq", manager.currentSequence());

    writer.key("bridge");
    writer.beginObject();
    writer.field("host", bridgeHost);
    writer.field("version", kBridgeVersionA);
    writer.field("uptime_sec", uptimeSec);
    writer.field("claude_desktop", claudeDesktopRunning ? "running" : "not_running");
    writer.field("cpu_pct", cpuPercent);
    writer.field("rss_mb", rssMb);
    writer.endObject();

    const Summary summary = manager.BuildSummary();
    writer.key("summary");
    writer.beginObject();
    writer.field("active", static_cast<int64_t>(summary.active));
    writer.field("working", static_cast<int64_t>(summary.working));
    writer.field("waiting", static_cast<int64_t>(summary.waiting));
    writer.field("idle", static_cast<int64_t>(summary.idle));
    writer.field("finished", static_cast<int64_t>(summary.finished));
    writer.field("error", static_cast<int64_t>(summary.error));
    writer.endObject();

    writer.key("sessions");
    writer.beginArray();
    for (const auto& [sessionId, session] : manager.sessions()) {
        WriteSession(writer, session, maxEventsPerSession);
    }
    writer.endArray();

    // Вузли — необов'язкове розширення. Немає жодного — немає й поля:
    // застосунки старіших версій не мають бачити нічого нового.
    if (nodes != nullptr && !nodes->empty()) {
        writer.key("nodes");
        writer.beginArray();
        for (const NodeResult& node : *nodes) {
            writer.beginObject();
            writer.field("id", node.id);
            writer.field("name", node.name);
            writer.field("status", node.status);
            writer.field("updated_at", node.updatedAtMs);
            if (node.error != NodeError::None) writer.field("error", ToString(node.error));
            if (!node.text.empty()) writer.field("text", node.text);
            if (!node.lines.empty()) {
                writer.key("lines");
                writer.beginArray();
                for (const NodeLine& line : node.lines) {
                    writer.beginObject();
                    writer.field("label", line.label);
                    writer.field("value", line.value);
                    writer.endObject();
                }
                writer.endArray();
            }
            writer.endObject();
        }
        writer.endArray();
    }

    // Завершені задачі передаються окремим переліком: користувач бачить,
    // що робота скінчилася, але вони не змішуються з активними.
    writer.key("finished");
    writer.beginArray();
    for (const SessionState& session : manager.finished()) {
        writer.beginObject();
        writer.field("sid", session.sessionId);
        writer.field("short", session.sessionId.substr(0, 8));
        writer.field("title", session.title);
        writer.field("project", session.project);
        writer.field("state", ToString(session.state));
        writer.field("started_at", session.startedAtMs);
        writer.field("last_update", session.lastUpdateMs);
        writer.endObject();
    }
    writer.endArray();

    writer.endObject();
    return out;
}

std::string BuildEventsJson(const std::vector<Event>& events) {
    std::string out;
    json::Writer writer(out);

    writer.beginObject();
    writer.field("t", "events");
    writer.key("items");
    writer.beginArray();
    for (const Event& event : events) WriteEvent(writer, event, kMaxEventText);
    writer.endArray();
    writer.endObject();

    return out;
}

std::string EncryptToForwardFrame(const crypto::SessionKey& key,
                                  crypto::NonceCounter& counter,
                                  std::string_view plaintextJson,
                                  std::string_view to) {
    uint8_t nonce[crypto::kGcmNonceSize]{};
    if (!counter.Next(nonce)) {
        // Простір лічильника вичерпано. Повторне використання nonce
        // розкрило б відкритий текст, тому відмовляємось, а не йдемо
        // на друге коло (docs/protocol.md §5).
        Log().Error("простір лічильника nonce вичерпано, потрібне повторне узгодження ключа");
        return {};
    }

    std::vector<uint8_t> ciphertext;
    if (!crypto::AesGcmEncrypt(key, nonce, sizeof(nonce), plaintextJson, ciphertext)) {
        Log().Error("не вдалося зашифрувати корисне навантаження");
        return {};
    }

    return MakeForwardFrame(Base64UrlEncode(nonce, sizeof(nonce)),
                            Base64UrlEncode(ciphertext),
                            to);
}

bool DecryptForwardPayload(const crypto::SessionKey& key,
                           std::string_view nonceB64,
                           std::string_view ciphertextB64,
                           std::string& plaintextJson) {
    std::vector<uint8_t> nonce;
    if (!Base64UrlDecode(nonceB64, nonce)) return false;
    if (nonce.size() != crypto::kGcmNonceSize) return false;

    std::vector<uint8_t> ciphertext;
    if (!Base64UrlDecode(ciphertextB64, ciphertext)) return false;
    if (ciphertext.size() < crypto::kGcmTagSize) return false;
    if (ciphertext.size() > kMaxFrameBytes) return false;

    // Невдача тут означає або пошкодження, або підміну. Обидва випадки
    // однаково неприйнятні, тож результат просто відкидається.
    return crypto::AesGcmDecrypt(key, nonce.data(), nonce.size(),
                                 ciphertext.data(), ciphertext.size(), plaintextJson);
}

}  // namespace cm
