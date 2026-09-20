// transport.cpp — WebSocket over TLS через WinHTTP.

#include "transport.h"
#include "crypto.h"
#include "logging.h"

#include <winhttp.h>

#include <algorithm>

namespace cm {

namespace {

constexpr DWORD kResolveTimeoutMs = 10'000;
constexpr DWORD kConnectTimeoutMs = 15'000;
constexpr DWORD kSendTimeoutMs    = 15'000;
constexpr DWORD kReceiveTimeoutMs = 60'000;

/// Максимальний розмір кадру, який приймаємо. Усе більше — відхиляється
/// до виділення пам'яті (Частина 4 §21 Master Prompt).
constexpr size_t kReceiveChunk = 8 * 1024;

}  // namespace

// ── RelayEndpoint ────────────────────────────────────────────────────────────

bool RelayEndpoint::Parse(const std::wstring& url, RelayEndpoint& out, bool allowInsecure) {
    if (url.empty()) return false;

    std::wstring rest;
    if (url.rfind(L"wss://", 0) == 0) {
        out.secure = true;
        out.port = 443;
        rest = url.substr(6);
    } else if (url.rfind(L"ws://", 0) == 0) {
        // Незахищений транспорт існує лише для локальної розробки і
        // вимагає явного дозволу. Автоматичного відкату на ws:// немає
        // і бути не може (Частина 3 §23 Master Prompt).
        if (!allowInsecure) {
            Log().Error("адреса ws:// відхилена: незашифрований транспорт заборонено");
            return false;
        }
        out.secure = false;
        out.port = 80;
        rest = url.substr(5);
    } else {
        Log().Error("адреса Relay має починатися з wss://");
        return false;
    }

    if (rest.empty()) return false;

    const size_t slash = rest.find(L'/');
    std::wstring authority = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == std::wstring::npos) ? L"/ws" : rest.substr(slash);
    if (out.path.empty()) out.path = L"/ws";

    // Порт, якщо вказаний. Квадратні дужки IPv6 тут не підтримуються
    // навмисно: Relay адресується доменним іменем.
    const size_t colon = authority.rfind(L':');
    if (colon != std::wstring::npos) {
        const std::wstring portText = authority.substr(colon + 1);
        bool numeric = !portText.empty();
        for (wchar_t c : portText) {
            if (c < L'0' || c > L'9') { numeric = false; break; }
        }
        if (numeric) {
            const unsigned long value = std::wcstoul(portText.c_str(), nullptr, 10);
            if (value == 0 || value > 65535) return false;
            out.port = static_cast<uint16_t>(value);
            authority = authority.substr(0, colon);
        }
    }

    if (authority.empty()) return false;
    out.host = authority;
    return true;
}

// ── WebSocketClient ──────────────────────────────────────────────────────────

WebSocketClient::WebSocketClient() {
    receiveBuffer_.resize(kReceiveChunk);
}

WebSocketClient::~WebSocketClient() {
    Abort();
}

void WebSocketClient::SetError(std::string_view message, DWORD code) {
    lastError_.assign(message);
    if (code != 0) {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), " (код %lu)", code);
        lastError_ += suffix;
    }
}

void WebSocketClient::CloseHandles() {
    if (socket_)     { ::WinHttpCloseHandle(socket_);     socket_ = nullptr; }
    if (request_)    { ::WinHttpCloseHandle(request_);    request_ = nullptr; }
    if (connection_) { ::WinHttpCloseHandle(connection_); connection_ = nullptr; }
    if (session_)    { ::WinHttpCloseHandle(session_);    session_ = nullptr; }
}

bool WebSocketClient::Connect(const RelayEndpoint& endpoint) {
    Abort();
    state_ = TransportState::Connecting;
    lastError_.clear();

    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY змушує WinHTTP підхопити системні
    // налаштування проксі. Без цього Bridge не працював би в корпоративних
    // мережах, де прямий вихід закритий.
    session_ = ::WinHttpOpen(L"claude-monitor-bridge/1.0",
                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) {
        SetError("не вдалося створити сеанс WinHTTP", ::GetLastError());
        state_ = TransportState::Disconnected;
        return false;
    }

    ::WinHttpSetTimeouts(session_, kResolveTimeoutMs, kConnectTimeoutMs,
                         kSendTimeoutMs, kReceiveTimeoutMs);

    // Обмежуємо протокол сучасними версіями TLS. Старіші мають відомі вади
    // і заборонені Частиною 3 §2 Master Prompt.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    ::WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS,
                       &protocols, sizeof(protocols));

    connection_ = ::WinHttpConnect(session_, endpoint.host.c_str(),
                                   static_cast<INTERNET_PORT>(endpoint.port), 0);
    if (!connection_) {
        SetError("не вдалося підключитися до вузла", ::GetLastError());
        CloseHandles();
        state_ = TransportState::Disconnected;
        return false;
    }

    const DWORD requestFlags = endpoint.secure ? WINHTTP_FLAG_SECURE : 0;
    request_ = ::WinHttpOpenRequest(connection_, L"GET", endpoint.path.c_str(),
                                    nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, requestFlags);
    if (!request_) {
        SetError("не вдалося створити запит", ::GetLastError());
        CloseHandles();
        state_ = TransportState::Disconnected;
        return false;
    }

    // Перевірка сертифіката лишається такою, як її робить система:
    // жодних послаблень через WINHTTP_OPTION_SECURITY_FLAGS тут немає.
    // Це навмисно — саме тут найчастіше й з'являються діри.

    if (!::WinHttpSetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        SetError("WebSocket не підтримується цією версією Windows", ::GetLastError());
        CloseHandles();
        state_ = TransportState::Disconnected;
        return false;
    }

    if (!::WinHttpSendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        const DWORD error = ::GetLastError();
        // Найчастіша причина саме тут — невдала перевірка сертифіката.
        // Це не привід щось вимикати: це привід повідомити користувача.
        if (error == ERROR_WINHTTP_SECURE_FAILURE) {
            SetError("перевірка сертифіката TLS не пройдена", error);
        } else {
            SetError("не вдалося надіслати запит на оновлення протоколу", error);
        }
        CloseHandles();
        state_ = TransportState::Disconnected;
        return false;
    }

    if (!::WinHttpReceiveResponse(request_, nullptr)) {
        SetError("Relay не відповів на запит оновлення протоколу", ::GetLastError());
        CloseHandles();
        state_ = TransportState::Disconnected;
        return false;
    }

    // Перевіряємо код відповіді: 101 означає успішне оновлення до WebSocket.
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (::WinHttpQueryHeaders(request_,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                              WINHTTP_NO_HEADER_INDEX)) {
        if (statusCode != 101) {
            char message[96];
            std::snprintf(message, sizeof(message),
                          "Relay відхилив підключення, код HTTP %lu", statusCode);
            SetError(message);
            CloseHandles();
            state_ = TransportState::Disconnected;
            return false;
        }
    }

    socket_ = ::WinHttpWebSocketCompleteUpgrade(request_, 0);
    if (!socket_) {
        SetError("не вдалося завершити оновлення до WebSocket", ::GetLastError());
        CloseHandles();
        state_ = TransportState::Disconnected;
        return false;
    }

    // Дескриптор запиту після успішного оновлення більше не потрібен.
    ::WinHttpCloseHandle(request_);
    request_ = nullptr;

    state_ = TransportState::Connected;
    Log().Debug("з'єднання з Relay встановлено");
    return true;
}

bool WebSocketClient::SendText(std::string_view payload) {
    if (!socket_ || state_ != TransportState::Connected) return false;

    // Кадр понад ліміт не надсилається: це означало б помилку формування
    // вище за течією, і краще виявити її тут, ніж отримати розрив від Relay.
    if (payload.size() > kMaxFrameBytes) {
        SetError("кадр перевищує допустимий розмір");
        return false;
    }

    const DWORD result = ::WinHttpWebSocketSend(
        socket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()));

    if (result != NO_ERROR) {
        SetError("не вдалося надіслати кадр", result);
        state_ = TransportState::Disconnected;
        return false;
    }

    bytesSent_ += payload.size();
    return true;
}

bool WebSocketClient::Receive(std::string& out, uint32_t timeoutMs) {
    if (!socket_ || state_ != TransportState::Connected) return false;

    out.clear();

    // Таймаут читання встановлюється на дескрипторі: WinHttpWebSocketReceive
    // блокує до появи даних, і без цього потік завис би до самого розриву.
    //
    // УВАГА: у WinHTTP значення 0 означає НЕСКІНЧЕННЕ очікування, а не
    // «не блокувати». Тому нуль замінюється на найменший ненульовий
    // таймаут — інакше спроба неблокуючого опитування підвішувала б
    // увесь головний цикл.
    DWORD receiveTimeout = (timeoutMs == 0) ? 1 : timeoutMs;
    ::WinHttpSetOption(socket_, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                       &receiveTimeout, sizeof(receiveTimeout));

    // Одне логічне повідомлення може прийти кількома фрагментами.
    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType{};

        const DWORD result = ::WinHttpWebSocketReceive(
            socket_, receiveBuffer_.data(), static_cast<DWORD>(receiveBuffer_.size()),
            &bytesRead, &bufferType);

        if (result != NO_ERROR) {
            if (result == ERROR_WINHTTP_TIMEOUT) return false;  // це не помилка

            SetError("помилка читання кадру", result);
            state_ = TransportState::Disconnected;
            return false;
        }

        switch (bufferType) {
            case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
                Log().Debug("Relay закрив з'єднання");
                state_ = TransportState::Disconnected;
                return false;

            case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
            case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
                // Протокол текстовий; двійкові кадри не передбачені.
                SetError("отримано двійковий кадр, який протокол не передбачає");
                state_ = TransportState::Disconnected;
                return false;

            default:
                break;
        }

        // Захист від нескінченного накопичення: зловмисний або зіпсований
        // потік фрагментів не має вичерпати пам'ять.
        if (out.size() + bytesRead > kMaxFrameBytes) {
            SetError("вхідне повідомлення перевищило допустимий розмір");
            state_ = TransportState::Disconnected;
            return false;
        }

        out.append(reinterpret_cast<const char*>(receiveBuffer_.data()), bytesRead);
        bytesReceived_ += bytesRead;

        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
        // Інакше це фрагмент — читаємо далі.
    }

    return !out.empty();
}

void WebSocketClient::Close(uint16_t statusCode, std::string_view reason) {
    if (socket_ && state_ == TransportState::Connected) {
        state_ = TransportState::Closing;

        // Коротка причина закриття: специфікація WebSocket обмежує її
        // 123 байтами.
        const std::string safeReason(reason.substr(0, 100));

        ::WinHttpWebSocketClose(socket_, statusCode,
                                safeReason.empty() ? nullptr
                                                   : const_cast<char*>(safeReason.data()),
                                static_cast<DWORD>(safeReason.size()));
    }

    CloseHandles();
    state_ = TransportState::Disconnected;
}

void WebSocketClient::Abort() {
    CloseHandles();
    state_ = TransportState::Disconnected;
}

// ── Backoff ──────────────────────────────────────────────────────────────────

uint32_t Backoff::NextDelayMs() {
    // 1, 2, 4, 8, 16, 32, 60 (стеля) секунд.
    const uint32_t shift = std::min<uint32_t>(attempts_, 5);
    uint32_t base = 1000u << shift;
    base = std::min(base, kMaxDelayMs);

    ++attempts_;

    // Відхилення ±20%. Без нього після падіння Relay усі клієнти
    // повернулися б одночасно й повалили б його знову.
    uint32_t jitterRange = base / 5;
    if (jitterRange == 0) return base;

    uint32_t jitter = 0;
    if (!crypto::RandomBelow(jitterRange * 2, jitter)) {
        // Джерело випадковості недоступне — краще без відхилення,
        // ніж без паузи взагалі.
        return base;
    }

    return base - jitterRange + jitter;
}

void Backoff::Reset() {
    attempts_ = 0;
}

}  // namespace cm
