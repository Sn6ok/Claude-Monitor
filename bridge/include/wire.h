// wire.h — формування та розбір кадрів протоколу.
//
// Реалізує обидва рівні з docs/protocol.md:
//   рівень 1 — кадри до Relay (hello, auth, fwd, ping…), він їх читає;
//   рівень 2 — прикладні повідомлення (snapshot, events), зашифровані
//              наскрізно, тож Relay їх не бачить.

#pragma once

#include "common.h"
#include "crypto.h"
#include "events.h"
#include "json.h"
#include "nodes.h"
#include "state.h"

namespace cm {

/// Генерує ідентифікатор кадру у форматі ULID (26 символів).
std::string MakeUlid();

// ── Рівень 1 ─────────────────────────────────────────────────────────────────

/// Загортає корисне навантаження в кадр протоколу.
std::string MakeFrame(std::string_view type, std::string_view payloadJson);

std::string MakeHelloFrame(std::string_view deviceId);
std::string MakeAuthFrame(std::string_view deviceId, std::string_view signatureB64);
std::string MakePingFrame();

std::string MakePairOfferFrame(std::string_view offerId,
                               std::string_view publicSign,
                               std::string_view publicExchange,
                               std::string_view confirm);

std::string MakePairOkFrame(std::string_view offerId, bool accepted);

/// Кадр маршрутизації із зашифрованим корисним навантаженням.
/// @param to адресат (device_id телефона); порожній — усім партнерам
std::string MakeForwardFrame(std::string_view nonceB64,
                             std::string_view ciphertextB64,
                             std::string_view to = {});

/// Розібраний вхідний кадр рівня 1.
struct IncomingFrame {
    std::string type;
    json::Value payload;
    bool        valid = false;
};

/// Розбирає та перевіряє вхідний кадр.
///
/// Перевірки йдуть від найдешевших до найдорожчих, як у docs/protocol.md §11:
/// розмір -> JSON -> версія -> тип -> час. Некоректний кадр ніколи не
/// призводить до винятку чи виділення великої пам'яті.
IncomingFrame ParseIncomingFrame(std::string_view raw, uint64_t nowMs);

/// Рядок, який підписується під час автентифікації.
std::string AuthSigningString(std::string_view role,
                              std::string_view deviceId,
                              std::string_view nonceB64);

// ── Рівень 2 ─────────────────────────────────────────────────────────────────

/// Формує повний знімок стану всіх задач.
std::string BuildSnapshotJson(const SessionManager& manager,
                              std::string_view bridgeHost,
                              uint64_t uptimeSec,
                              bool claudeDesktopRunning,
                              double cpuPercent,
                              double rssMb,
                              size_t maxEventsPerSession = kMaxSnapshotEvents,
                              const std::vector<NodeResult>* nodes = nullptr);

/// Формує пакет подій.
std::string BuildEventsJson(const std::vector<Event>& events);

/// Шифрує прикладне повідомлення й загортає його в кадр `fwd`.
/// @param to адресат (device_id телефона); порожній — усім партнерам
/// @returns порожній рядок, якщо шифрування не вдалося
std::string EncryptToForwardFrame(const crypto::SessionKey& key,
                                  crypto::NonceCounter& counter,
                                  std::string_view plaintextJson,
                                  std::string_view to = {});

/// Розшифровує корисне навантаження кадру `fwd`.
bool DecryptForwardPayload(const crypto::SessionKey& key,
                           std::string_view nonceB64,
                           std::string_view ciphertextB64,
                           std::string& plaintextJson);

}  // namespace cm
