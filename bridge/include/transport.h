// transport.h — захищений транспорт до Relay через WinHTTP WebSocket.
//
// Чому WinHTTP, а не окрема бібліотека (Частина 6 §6 Master Prompt):
//   * входить до складу Windows — нуль зовнішніх залежностей;
//   * перевірка сертифікатів TLS виконується системою, з її сховищем
//     довірених центрів і списками відкликання;
//   * автоматично використовує системні налаштування проксі;
//   * підтримує WebSocket нативно, починаючи з Windows 8.
//
// Перевірка сертифіката НІКОЛИ не вимикається. Жодного «прийняти будь-який
// сертифікат» тут немає і бути не може (Частина 3 §2 Master Prompt).

#pragma once

#include "common.h"

#include <functional>
#include <string>

namespace cm {

enum class TransportState : uint8_t {
    Disconnected = 0,
    Connecting,
    Connected,
    Closing,
};

/// Розібрана адреса Relay.
struct RelayEndpoint {
    std::wstring host;
    std::wstring path;
    uint16_t     port = 443;
    bool         secure = true;

    /// Розбирає адресу виду wss://host[:port]/path
    /// @param allowInsecure дозволити ws:// (лише для локальної розробки)
    static bool Parse(const std::wstring& url, RelayEndpoint& out, bool allowInsecure);
};

/// Клієнт WebSocket поверх TLS.
///
/// Клас НЕ є потокобезпечним і призначений для роботи всередині одного
/// мережевого потоку. Це свідоме спрощення: уся взаємодія з рештою Bridge
/// іде через чергу подій, тож спільного стану немає.
class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    /// Встановлює з'єднання. Блокує викликача до завершення або таймауту.
    bool Connect(const RelayEndpoint& endpoint);

    /// Надсилає текстовий кадр UTF-8.
    bool SendText(std::string_view payload);

    /// Отримує наступний кадр.
    ///
    /// @param out       сюди складається корисне навантаження
    /// @param timeoutMs максимальний час очікування
    /// @returns true, якщо кадр отримано; false — таймаут, закриття чи помилка
    bool Receive(std::string& out, uint32_t timeoutMs);

    /// Коректно закриває з'єднання.
    void Close(uint16_t statusCode = 1000, std::string_view reason = {});

    /// Негайно звільняє ресурси без обміну кадрами закриття.
    void Abort();

    TransportState state() const { return state_; }
    bool connected() const { return state_ == TransportState::Connected; }

    /// Причина останньої невдачі — для журналу. Без деталей винятків.
    const std::string& lastError() const { return lastError_; }

    uint64_t bytesSent() const { return bytesSent_; }
    uint64_t bytesReceived() const { return bytesReceived_; }

private:
    void SetError(std::string_view message, DWORD code = 0);
    void CloseHandles();

    void* session_ = nullptr;    // HINTERNET
    void* connection_ = nullptr; // HINTERNET
    void* request_ = nullptr;    // HINTERNET
    void* socket_ = nullptr;     // HINTERNET (WebSocket)

    TransportState state_ = TransportState::Disconnected;
    std::string    lastError_;
    std::vector<uint8_t> receiveBuffer_;

    uint64_t bytesSent_ = 0;
    uint64_t bytesReceived_ = 0;
};

/// Обчислення пауз між спробами перепідключення.
///
/// Стратегія з docs/protocol.md §10: 1, 2, 4, 8, 16, 30, 60 секунд зі
/// стелею та випадковим відхиленням. Відхилення обов'язкове — інакше після
/// падіння Relay усі клієнти повернулися б синхронно й створили сплеск.
class Backoff {
public:
    /// @returns пауза в мілісекундах перед наступною спробою
    uint32_t NextDelayMs();

    /// Скидається лише після УСПІШНОЇ автентифікації, а не після
    /// встановлення TCP-з'єднання. Інакше цикл «підключився — відмовили
    /// в авторизації» перетворився б на агресивний потік спроб
    /// (Частина 5 §20 Master Prompt).
    void Reset();

    uint32_t attempts() const { return attempts_; }

private:
    static constexpr uint32_t kMaxDelayMs = 60'000;
    uint32_t attempts_ = 0;
};

}  // namespace cm
