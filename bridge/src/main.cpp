// main.cpp — точка входу Windows Bridge.
//
// Режими запуску:
//   (без аргументів)      робочий режим: спостереження та передача подій
//   --hook <подія>        короткий виклик від Claude Code; передає дані
//                         робочому Bridge і одразу завершується
//   --pair                показує код pairing і чекає на телефон (до 5 телефонів)
//   --status              стан і діагностика
//   --install-hooks       друкує фрагмент для ~/.claude/settings.json
//   --set-relay <url>     зберігає адресу Relay
//   --unpair [id]         від'єднує телефон за початком ідентифікатора,
//                         без ідентифікатора — усі телефони
//
// Життєвий цикл (Частина 5 §3, §4 Master Prompt): Bridge НЕ запускається
// разом із Windows. Його піднімає hook SessionStart, а завершується він
// разом із Claude Desktop — очікуванням на дескрипторі процесу.

#include "common.h"
#include "config.h"
#include "hookpipe.h"
#include "lifecycle.h"
#include "logging.h"
#include "nodes.h"
#include "pairing.h"
#include "queue.h"
#include "sessions.h"
#include "state.h"
#include "transport.h"
#include "watcher.h"
#include "wire.h"

#include <iterator>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace cm {

namespace {

/// Запобіжний таймер основного циклу: цикл прокидається від подій файлової
/// системи, а таймер — на випадок, коли повідомлення запізнилося чи загубилося.
constexpr uint32_t kMainLoopTimeoutMs = 2'000;

/// Як часто дочитувати транскрипти незалежно від повідомлень файлової системи.
///
/// Windows повідомляє про зміну файлу, лише коли оновлює його метадані,
/// а для файлу, який Claude Code тримає відкритим, це може відкладатися
/// на хвилини. Без цього дочитування відповідь Claude вже була записана,
/// а на телефоні з'являлася лише за кілька хвилин. Перевірка дешева:
/// відкрити файл і порівняти розмір із прочитаним.
constexpr uint64_t kTranscriptPollMs = 2'000;

/// Період надсилання ping до Relay за відсутності іншого обміну.
constexpr uint64_t kPingIntervalMs = 45'000;

/// Як часто оновлювати стан на телефоні, коли подій немає.
///
/// Значення менше за поріг, після якого застосунок вважає дані застарілими,
/// інакше в спокійні періоди він хибно показував би «дані застаріли».
constexpr uint64_t kIdleRefreshMs = 20'000;

/// Як часто перевіряти, чи не змінився перелік телефонів на диску:
/// --pair і --unpair працюють окремим процесом.
constexpr uint64_t kPhonesCheckMs = 5'000;

/// Як часто шукати Claude Desktop, коли його вікно закрили.
constexpr uint64_t kDesktopCheckMs = 30'000;

/// Скільки працювати без Claude Desktop, якщо не лишилося й задач.
///
/// Сесії Claude Code переживають закриття вікна Claude Desktop, і раніше
/// Bridge ішов слідом за вікном: телефон лишався із застиглим чатом, хоча
/// робота тривала. Тепер Bridge іде, лише коли стежити справді нема за чим.
constexpr uint64_t kOrphanGraceMs = 5 * 60 * 1000;

/// Максимум подій в одному пакеті до телефона.
// Спільна межа з common.h, а не власне число: подія тепер несе до 3 КБ
// тексту, і 32 таких події в одному пакеті переповнили б кадр.
constexpr size_t kEventsPerBatch = kMaxEventsPerBatch;

/// Скільки байтів займе кадр fwd із відкритим текстом такого розміру:
/// шифрування додає 16 байтів тегу, base64url — третину, обгортка кадру
/// разом з адресатом — до трьох сотень байтів.
size_t ForwardFrameBytes(size_t plaintextBytes) {
    return (plaintextBytes + 16) * 4 / 3 + 4 + 300;
}

/// Затримка перед відправленням пакета. Дає дрібним подіям згрупуватися
/// в один кадр, не створюючи помітної для користувача паузи
/// (Частина 4 §20 Master Prompt).
constexpr uint64_t kBatchDelayMs = 250;

std::atomic<bool> g_shutdownRequested{false};

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_BREAK_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
        g_shutdownRequested = true;
        return TRUE;
    }
    return FALSE;
}

void EnableUtf8Console() {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    // Рядкова буферизація: повідомлення з'являються одразу, навіть коли
    // вивід перенаправлено у файл. Для інтерактивних режимів це критично —
    // користувач має бачити код pairing негайно.
    ::setvbuf(stdout, nullptr, _IOLBF, 4096);
}

std::wstring HostName() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (::GetComputerNameW(buffer, &size)) return std::wstring(buffer, size);
    return L"windows";
}

/// Власне споживання пам'яті — для показу в застосунку та бенчмарків.
double SelfRssMb() {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) return 0.0;
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
}

/// Відсоток процесорного часу з моменту попереднього виклику.
double SelfCpuPercent() {
    static ULONGLONG lastKernel = 0, lastUser = 0, lastTime = 0;

    FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
    if (!::GetProcessTimes(::GetCurrentProcess(), &creation, &exitTime, &kernelTime, &userTime)) {
        return 0.0;
    }

    auto toU64 = [](const FILETIME& ft) {
        ULARGE_INTEGER value{};
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return value.QuadPart;
    };

    const ULONGLONG kernel = toU64(kernelTime);
    const ULONGLONG user = toU64(userTime);
    const ULONGLONG now = NowMonotonicMs();

    if (lastTime == 0) {
        lastKernel = kernel; lastUser = user; lastTime = now;
        return 0.0;
    }

    const ULONGLONG elapsedMs = now - lastTime;
    if (elapsedMs == 0) return 0.0;

    // Час процесу вимірюється в 100-наносекундних інтервалах.
    const ULONGLONG usedMs = ((kernel - lastKernel) + (user - lastUser)) / 10'000;

    lastKernel = kernel; lastUser = user; lastTime = now;

    const double percent = (static_cast<double>(usedMs) / static_cast<double>(elapsedMs)) * 100.0;

    // Кількість ядер беремо з Win32, щоб не тягнути <thread> заради
    // одного виклику. Значення стале, тож рахуємо його один раз.
    static const DWORD cores = []() -> DWORD {
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        return info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1;
    }();

    return percent / static_cast<double>(cores);
}

/// Мітка останньої зміни файлу. 0 — файлу немає.
uint64_t FileWriteStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
}

/// Короткий вигляд ідентифікатора для журналу.
std::string ShortId(std::string_view id) {
    return std::string(id.substr(0, 8));
}

}  // namespace

// ── Робочий режим ────────────────────────────────────────────────────────────

/// Основний клас Bridge.
///
/// Розділення на два потоки — вимога архітектури, а не оптимізація
/// (Частина 5 §15, §19 Master Prompt):
///
///   потік СПОСТЕРЕЖЕННЯ  читає транскрипти, наповнює чергу; ніколи
///                        не торкається мережі й не може на ній зависнути;
///   потік МЕРЕЖІ         підключається, автентифікується, надсилає події
///                        та читає відповіді блокуючими викликами.
///
/// Односпрямований потік даних через захищену чергу означає, що обрив
/// зв'язку чи зависання Relay фізично не можуть уповільнити читання
/// транскриптів, а отже — не можуть вплинути на Claude Code.
class BridgeApp {
public:
    BridgeApp(Config& config, Identity& identity)
        : config_(config),
          identity_(identity),
          manager_([this](Event&& event) { OnEvent(std::move(event)); }),
          nodes_(config.dataDir + L"\\nodes") {}

    int Run();

private:
    // ── Потік спостереження ──────────────────────────────────────────────
    void OnEvent(Event&& event);
    void HandleHookMessage(const HookMessage& message);
    void RescanSessions();
    void RefreshSnapshotPayload();

    // ── Потік мережі ─────────────────────────────────────────────────────
    static DWORD WINAPI NetworkThreadEntry(LPVOID parameter);
    void NetworkLoop();
    bool ConnectAndAuthenticate(WebSocketClient& socket);
    void ServeConnection(WebSocketClient& socket);
    void HandleIncoming(WebSocketClient& socket, const std::string& raw);

    // ── Телефони ─────────────────────────────────────────────────────────
    //
    // До ноутбука можна підключити кілька телефонів. Кожен має власний ключ
    // шифрування й отримує власні кадри: те, що зашифровано для одного
    // телефона, інший не прочитає.

    /// Спарений телефон разом із ключем і станом доставки.
    struct PhoneLink {
        PairedPhone          phone;
        crypto::SessionKey   key{};
        crypto::NonceCounter counter{"B2M "};
        bool                 online = false;
        bool                 wantsSnapshot = true;
    };

    /// Перебудовує ключі за identity_.phones. Викликати під linksLock_.
    void RebuildLinksLocked();

    /// Перечитує перелік телефонів із диска: pairing виконується окремим
    /// процесом, і робочий Bridge інакше не знав би ключа нового телефона.
    /// @returns true, якщо перелік змінився
    bool ReloadPhones();

    bool HasPhones();

    /// Позначає телефон у мережі чи поза нею. Порожній id — усі телефони.
    /// @returns false, якщо телефона з таким id немає
    bool SetPhoneOnline(std::string_view phoneId, bool online);

    /// Телефон отримав знімок — відтепер йому йдуть і події.
    void MarkSnapshotDelivered(std::string_view phoneId);

    /// Relay відкликав сам Bridge: жоден телефон більше нічого не отримає.
    void ForgetAllPhones();

    // ── Потік читання ────────────────────────────────────────────────────
    struct ReaderContext {
        BridgeApp* app;
        WebSocketClient* socket;
        std::atomic<bool>& active;
    };

    static DWORD WINAPI ReaderThreadEntry(LPVOID parameter);
    void ReaderLoop(WebSocketClient& socket, std::atomic<bool>& active);

    Config&        config_;
    Identity&      identity_;
    SessionManager manager_;
    EventQueue     queue_;
    SessionScanner scanner_;
    WatchSet       watch_;
    HookPipeServer hookServer_;

    /// Вузли: розширення користувача. Живуть у власному потоці й на решту
    /// Bridge не впливають — навіть якщо вузол зависне.
    NodeRunner     nodes_;

    // Телефони читає потік читання (розшифрування, присутність) і потік
    // мережі (шифрування, відправлення), тож доступ — під замком.
    Lock                   linksLock_;
    std::vector<PhoneLink> links_;
    std::atomic<uint64_t>  phonesStamp_{0};

    RelayEndpoint endpoint_;
    Handle        desktopProcess_;
    uint32_t      desktopPid_ = 0;

    BridgeState state_ = BridgeState::Starting;
    uint64_t    startedAtMs_ = 0;

    // ── Спільний стан двох потоків ───────────────────────────────────────
    //
    // Знімок готує потік спостереження (лише він має право читати
    // SessionManager), а надсилає потік мережі. Обмін іде через рядок
    // під замком: так мережевий потік ніколи не заглядає у структури,
    // які змінюються паралельно.
    Lock        snapshotLock_;
    std::string snapshotPayload_;

    /// Знімок потрібен усім телефонам у мережі (стан змінився, планове оновлення).
    std::atomic<bool> snapshotRequested_{true};
    std::atomic<bool> snapshotReady_{false};
    std::atomic<bool> peerOnline_{false};
    std::atomic<bool> networkRunning_{false};

    Handle networkThread_;
    Handle networkWakeEvent_;
};

void BridgeApp::OnEvent(Event&& event) {
    // Спостереження ніколи не чекає на мережу: подія лягає в чергу,
    // і потік одразу повертається до читання транскриптів
    // (Частина 5 §15, §19 Master Prompt).
    queue_.Push(std::move(event));
}

void BridgeApp::RescanSessions() {
    const auto discovered = scanner_.Scan();
    manager_.Reconcile(discovered);
}

void BridgeApp::HandleHookMessage(const HookMessage& message) {
    Log().Debugf("хук: %s", message.event.c_str());

    if (message.event == "session-start") {
        // Нова сесія: перескановуємо реєстр. Файл сесії міг ще не з'явитися
        // на диску, тож повторна перевірка станеться і за таймером.
        RescanSessions();

    } else if (message.event == "session-end") {
        if (!message.sessionId.empty()) manager_.MarkFinished(message.sessionId);

    } else if (message.event == "notification") {
        // Це єдиний надійний спосіб дізнатися, що Claude чекає на дію
        // користувача — саме той сценарій, заради якого будувався проєкт.
        if (!message.sessionId.empty()) {
            manager_.MarkWaiting(message.sessionId, message.text);
        } else {
            // Сесію не вказано: якщо активна лише одна, сигнал належить їй.
            // За кількох активних приписувати навмання не можна.
            if (manager_.sessions().size() == 1) {
                manager_.MarkWaiting(manager_.sessions().begin()->first, message.text);
            }
        }
    }
}

/// Готує знімок стану. Викликається ТІЛЬКИ з потоку спостереження:
/// лише він має право читати SessionManager.
void BridgeApp::RefreshSnapshotPayload() {
    const uint64_t uptimeSec = (NowMonotonicMs() - startedAtMs_) / 1000;
    const std::string host = Utf16ToUtf8(HostName());
    const double cpu = SelfCpuPercent();
    const double rss = SelfRssMb();

    // Знімок — один кадр на всі задачі. З кількома задачами й довгими
    // командами він міг би перерости межу кадру і тоді не дійшов би ніколи,
    // а без знімка телефон не отримує й подій. Тож за потреби беремо менше
    // останніх подій на задачу: стан важливіший за історію.
    const std::vector<NodeResult> nodeCards = nodes_.results();

    std::string payload;
    for (size_t eventsPerSession : {kMaxSnapshotEvents, size_t{10}, size_t{5}, size_t{0}}) {
        payload = BuildSnapshotJson(manager_, host, uptimeSec, desktopProcess_.valid(),
                                    cpu, rss, eventsPerSession, &nodeCards);
        if (ForwardFrameBytes(payload.size()) <= kMaxFrameBytes) break;
    }

    {
        Guard guard(snapshotLock_);
        snapshotPayload_ = std::move(payload);
    }
    snapshotReady_ = true;

    // Будимо мережевий потік: знімок готовий до відправлення.
    if (networkWakeEvent_) ::SetEvent(networkWakeEvent_.get());
}

DWORD WINAPI BridgeApp::NetworkThreadEntry(LPVOID parameter) {
    static_cast<BridgeApp*>(parameter)->NetworkLoop();
    return 0;
}

// ── Телефони ─────────────────────────────────────────────────────────────────

void BridgeApp::RebuildLinksLocked() {
    std::vector<PhoneLink> fresh;
    fresh.reserve(identity_.phones.size());

    for (const PairedPhone& phone : identity_.phones) {
        PhoneLink link;
        link.phone = phone;
        if (!identity_.DeriveSessionKey(phone, link.key)) {
            Log().Warnf("не вдалося обчислити ключ шифрування для телефона %s…",
                        ShortId(phone.deviceId).c_str());
            continue;
        }
        link.counter = crypto::NonceCounter::ForSession("B2M ");

        // Уже відомий телефон зберігає лічильник і стан: номери nonce
        // з тим самим ключем не можуть піти на повтор.
        for (const PhoneLink& old : links_) {
            if (old.phone.deviceId != phone.deviceId) continue;
            link.counter = old.counter;
            link.online = old.online;
            link.wantsSnapshot = old.wantsSnapshot;
        }
        fresh.push_back(std::move(link));
    }

    links_ = std::move(fresh);
    peerOnline_ = std::any_of(links_.begin(), links_.end(),
                              [](const PhoneLink& link) { return link.online; });
}

bool BridgeApp::ReloadPhones() {
    phonesStamp_ = FileWriteStamp(config_.peerPath());

    Guard guard(linksLock_);

    std::vector<std::string> before;
    for (const PhoneLink& link : links_) before.push_back(link.phone.deviceId);

    if (!identity_.LoadPhones(config_)) return false;

    std::vector<std::string> after;
    for (const PairedPhone& phone : identity_.phones) after.push_back(phone.deviceId);
    if (after == before) return false;

    for (const std::string& id : after) {
        if (std::find(before.begin(), before.end(), id) == before.end()) {
            Log().Infof("підключено телефон %s…", ShortId(id).c_str());
        }
    }
    for (const std::string& id : before) {
        if (std::find(after.begin(), after.end(), id) == after.end()) {
            Log().Infof("від'єднано телефон %s…", ShortId(id).c_str());
        }
    }

    RebuildLinksLocked();
    return true;
}

bool BridgeApp::HasPhones() {
    Guard guard(linksLock_);
    return !links_.empty();
}

bool BridgeApp::SetPhoneOnline(std::string_view phoneId, bool online) {
    Guard guard(linksLock_);

    bool found = false;
    for (PhoneLink& link : links_) {
        if (!phoneId.empty() && link.phone.deviceId != phoneId) continue;
        found = true;
        // Телефон щойно з'явився — йому потрібен повний стан.
        if (online && !link.online) link.wantsSnapshot = true;
        link.online = online;
    }

    peerOnline_ = std::any_of(links_.begin(), links_.end(),
                              [](const PhoneLink& link) { return link.online; });
    return found;
}

void BridgeApp::MarkSnapshotDelivered(std::string_view phoneId) {
    Guard guard(linksLock_);
    for (PhoneLink& link : links_) {
        if (link.phone.deviceId == phoneId) link.wantsSnapshot = false;
    }
}

void BridgeApp::ForgetAllPhones() {
    Guard guard(linksLock_);
    identity_.ForgetAllPhones();
    identity_.SavePhones(config_);
    links_.clear();
    peerOnline_ = false;
}

// ── Підключення до Relay ─────────────────────────────────────────────────────

bool BridgeApp::ConnectAndAuthenticate(WebSocketClient& socket) {
    if (!socket.Connect(endpoint_)) {
        Log().Warnf("не вдалося підключитися: %s", socket.lastError().c_str());
        return false;
    }

    if (!socket.SendText(MakeHelloFrame(identity_.deviceId))) return false;

    std::string raw;
    if (!socket.Receive(raw, 15'000)) {
        Log().Warn("Relay не надіслав виклик автентифікації");
        return false;
    }

    const IncomingFrame challenge = ParseIncomingFrame(raw, NowUnixMs());
    if (!challenge.valid || challenge.type != "challenge") {
        Log().Warn("очікувався виклик автентифікації");
        return false;
    }

    const std::string nonce = challenge.payload["nonce"].asStringOr("");
    if (nonce.empty()) return false;

    std::array<uint8_t, crypto::kSignatureSize> signature{};
    if (!identity_.signing.Sign(AuthSigningString("bridge", identity_.deviceId, nonce),
                                signature)) {
        Log().Error("не вдалося підписати виклик");
        return false;
    }

    if (!socket.SendText(MakeAuthFrame(identity_.deviceId,
                                       Base64UrlEncode(signature.data(), signature.size())))) {
        return false;
    }

    if (!socket.Receive(raw, 15'000)) return false;

    const IncomingFrame response = ParseIncomingFrame(raw, NowUnixMs());
    if (!response.valid) return false;

    if (response.type == "error") {
        const std::string code = response.payload["code"].asStringOr("");
        Log().Errorf("Relay відхилив автентифікацію: %s", code.c_str());

        if (code == "device_revoked") {
            // Доступ відкликано користувачем. Стираємо дані телефонів,
            // щоб не повторювати марних спроб (Частина 3 §21 Master Prompt).
            ForgetAllPhones();
        }
        return false;
    }
    if (response.type != "auth_ok") return false;

    // Котрі з телефонів у мережі. Relay старої версії знає лише «хтось у мережі».
    const bool anyOnline = response.payload["peer_online"].asBool(false);
    const json::Value& onlineList = response.payload["peers_online"];

    size_t onlineCount = 0;
    size_t total = 0;
    {
        Guard guard(linksLock_);
        for (PhoneLink& link : links_) {
            bool online = anyOnline;
            if (onlineList.isArray()) {
                online = false;
                for (const json::Value& id : onlineList.asArray()) {
                    if (id.asStringOr("") == link.phone.deviceId) online = true;
                }
            }
            link.online = online;
            // Після нового з'єднання кожному телефону в мережі — свіжий знімок.
            link.wantsSnapshot = online;
            if (online) ++onlineCount;
        }
        total = links_.size();
    }
    peerOnline_ = onlineCount > 0 || (anyOnline && !onlineList.isArray());

    Log().Infof("автентифіковано на Relay, телефонів у мережі: %zu з %zu", onlineCount, total);
    return true;
}

void BridgeApp::HandleIncoming(WebSocketClient& socket, const std::string& raw) {
    const IncomingFrame frame = ParseIncomingFrame(raw, NowUnixMs());
    if (!frame.valid) return;

    if (frame.type == "ping") { socket.SendText(MakeFrame("pong", "{}")); return; }
    if (frame.type == "pong") return;

    if (frame.type == "error") {
        const std::string code = frame.payload["code"].asStringOr("");
        const std::string phoneId = frame.payload["device_id"].asStringOr("");

        if (code == "peer_online" || code == "peer_offline") {
            const bool online = code == "peer_online";
            if (!SetPhoneOnline(phoneId, online) && online && !phoneId.empty()) {
                // Невідомий телефон — найімовірніше, його щойно спарили
                // окремим процесом, і робочий Bridge ще не знає його ключа.
                if (ReloadPhones()) SetPhoneOnline(phoneId, online);
            }
            Log().Debugf("телефон %s %s",
                         phoneId.empty() ? "?" : ShortId(phoneId).c_str(),
                         online ? "у мережі" : "поза мережею");
        } else if (code == "device_revoked") {
            ForgetAllPhones();
            Log().Warn("доступ пристрою відкликано");
        } else if (code != "not_paired") {
            Log().Warnf("Relay повідомив про помилку: %s", code.c_str());
        }
        return;
    }

    if (frame.type == "fwd") {
        const std::string from = frame.payload["from"].asStringOr("");
        const std::string nonceB64 = frame.payload["n"].asStringOr("");
        const std::string cipherB64 = frame.payload["ct"].asStringOr("");

        std::string plaintext;
        std::string sender;

        // Relay каже, від кого кадр, — ним і обирається ключ. Relay старої
        // версії цього не каже, тоді пробуємо ключі всіх телефонів.
        const auto tryDecrypt = [&]() {
            Guard guard(linksLock_);
            for (const PhoneLink& link : links_) {
                if (!from.empty() && link.phone.deviceId != from) continue;
                if (DecryptForwardPayload(link.key, nonceB64, cipherB64, plaintext)) {
                    sender = link.phone.deviceId;
                    return true;
                }
            }
            return false;
        };

        if (!tryDecrypt()) {
            // Найчастіша причина — не підробка, а новий телефон.
            //
            // Pairing виконується окремим процесом (`--pair`), який записує
            // новий телефон на диск. Робочий Bridge про це не знає, доки
            // не перечитає перелік, — тож перед тим як відкинути кадр,
            // перечитуємо й пробуємо ще раз.
            if (ReloadPhones() && tryDecrypt()) {
                Log().Info("ключі телефонів оновлено після нового підключення");
            } else {
                Log().Warnf("не вдалося розшифрувати повідомлення від %s",
                            from.empty() ? "невідомого пристрою" : (ShortId(from) + "…").c_str());
                return;
            }
        }

        json::Value request;
        if (!json::Parse(plaintext, request) || !request.isObject()) return;

        if (request["t"].asStringOr("") == "snapshot_request") {
            Guard guard(linksLock_);
            for (PhoneLink& link : links_) {
                if (link.phone.deviceId != sender) continue;
                // Телефон, що просить знімок, точно в мережі.
                link.online = true;
                link.wantsSnapshot = true;
            }
            peerOnline_ = true;
        }
        // Команди керування протоколом передбачені, але у версії 1
        // застосунок лише читає (Частина 4 §27 Master Prompt).
    }
}

/// Потік читання. Існує рівно на час життя одного з'єднання.
///
/// Винесений окремо тому, що WinHttpWebSocketReceive блокується до появи
/// кадру НЕЗАЛЕЖНО від заданого таймауту: WINHTTP_OPTION_RECEIVE_TIMEOUT
/// на дескриптор WebSocket не діє. В одному потоці з відправленням це
/// давало взаємне очікування — Bridge чекав кадр від телефона, телефон
/// чекав дані від Bridge, і обидва мовчали.
DWORD WINAPI BridgeApp::ReaderThreadEntry(LPVOID parameter) {
    auto* context = static_cast<ReaderContext*>(parameter);
    context->app->ReaderLoop(*context->socket, context->active);
    return 0;
}

void BridgeApp::ReaderLoop(WebSocketClient& socket, std::atomic<bool>& active) {
    while (active && networkRunning_ && socket.connected()) {
        std::string raw;
        if (!socket.Receive(raw, 30'000)) {
            if (!socket.connected()) break;
            continue;  // таймаут читання — не помилка
        }
        HandleIncoming(socket, raw);
    }
    active = false;
}

/// Обслуговує одне встановлене з'єднання до його розриву.
///
/// Цей потік лише ВІДПРАВЛЯЄ. Читання паралельно веде окремий потік.
void BridgeApp::ServeConnection(WebSocketClient& socket) {
    // Новий сеанс — нові лічильники nonce. Ключі між з'єднаннями сталі, тож
    // лічильник стартує з позначки часу й вище за вже видані номери: інакше
    // після кожного перепідключення nonce повторювались би (docs/protocol.md §5).
    {
        Guard guard(linksLock_);
        for (PhoneLink& link : links_) link.counter = crypto::NonceCounter::ForSession("B2M ");
    }

    uint64_t lastPingMs = NowMonotonicMs();
    uint64_t lastDataMs = NowMonotonicMs();
    uint64_t lastPhonesCheckMs = NowMonotonicMs();

    std::atomic<bool> readerActive{true};
    ReaderContext context{this, &socket, readerActive};

    Handle reader(::CreateThread(nullptr, 0, &BridgeApp::ReaderThreadEntry,
                                 &context, 0, nullptr));
    if (!reader) {
        Log().Error("не вдалося створити потік читання");
        return;
    }

    Log().Debugf("обслуговування розпочато: запит=%d, знімок готовий=%d",
                 snapshotRequested_ ? 1 : 0, snapshotReady_ ? 1 : 0);

    while (networkRunning_ && socket.connected() && readerActive) {
        // Пауза між перевірками. Відправлення більше не чекає на вхідні
        // кадри, тож знімок і події йдуть одразу, як тільки готові.
        ::Sleep(200);

        if (!socket.connected()) break;

        // Перелік телефонів міг змінитися: --pair і --unpair — окремі процеси.
        if (NowMonotonicMs() - lastPhonesCheckMs >= kPhonesCheckMs) {
            lastPhonesCheckMs = NowMonotonicMs();
            if (FileWriteStamp(config_.peerPath()) != phonesStamp_) ReloadPhones();
        }

        // ── Знімок стану ─────────────────────────────────────────────────
        //
        // Кожному телефону, що його чекає (щойно з'явився чи попросив),
        // а коли знімок потрібен усім — кожному телефону в мережі. Кадри
        // готуються під замком, а надсилаються вже без нього.
        if (snapshotReady_) {
            const bool forAll = snapshotRequested_.exchange(false);

            std::string payload;
            {
                Guard guard(snapshotLock_);
                payload = snapshotPayload_;
            }

            std::vector<std::pair<std::string, std::string>> frames;  // телефон, кадр
            if (payload.empty()) {
                if (forAll) Log().Warn("знімок стану порожній — нічого надсилати");
            } else {
                Guard guard(linksLock_);
                for (PhoneLink& link : links_) {
                    if (!link.online || !(forAll || link.wantsSnapshot)) continue;
                    std::string frame = EncryptToForwardFrame(link.key, link.counter, payload,
                                                              link.phone.deviceId);
                    if (frame.empty()) {
                        Log().Warn("не вдалося зашифрувати знімок стану");
                        continue;
                    }
                    frames.emplace_back(link.phone.deviceId, std::move(frame));
                }
            }

            bool failed = false;
            for (const auto& [phoneId, frame] : frames) {
                if (!socket.SendText(frame)) {
                    Log().Warnf("не вдалося надіслати знімок: %s", socket.lastError().c_str());
                    failed = true;
                    break;
                }
                MarkSnapshotDelivered(phoneId);
                lastDataMs = NowMonotonicMs();
                Log().Infof("надіслано знімок стану (%zu байтів) телефону %s…",
                            frame.size(), ShortId(phoneId).c_str());
            }
            if (failed && forAll) snapshotRequested_ = true;
        }

        // ── Події ────────────────────────────────────────────────────────
        //
        // Лише телефонам, які вже мають знімок: інакше телефон отримав би
        // зміни стану, не маючи самого стану.
        if (!queue_.empty()) {
            size_t online = 0;
            size_t ready = 0;
            {
                Guard guard(linksLock_);
                for (const PhoneLink& link : links_) {
                    if (!link.online) continue;
                    ++online;
                    if (!link.wantsSnapshot) ++ready;
                }
            }

            if (online == 0) {
                // Жоден телефон не в мережі — Relay однаково не мав би кому
                // доставити. Коли телефон з'явиться, він отримає знімок стану
                // з останніми подіями, а черга тим часом не розростається.
                std::vector<Event> dropped;
                queue_.Drain(dropped, kEventsPerBatch);
            } else if (ready > 0) {
                std::vector<Event> batch;
                if (queue_.Drain(batch, kEventsPerBatch) > 0) {
                    std::string payload = BuildEventsJson(batch);

                    // Пакет, що не вміщується в кадр, ділимо навпіл, а хвіст
                    // повертаємо в чергу. Без цього завеликий пакет вертався б
                    // у чергу знову й знову, і жодна наступна подія вже не дійшла б
                    // до телефона.
                    while (ForwardFrameBytes(payload.size()) > kMaxFrameBytes && batch.size() > 1) {
                        const size_t keep = batch.size() / 2;
                        std::vector<Event> tail(std::make_move_iterator(batch.begin() + keep),
                                                std::make_move_iterator(batch.end()));
                        batch.resize(keep);
                        queue_.Requeue(std::move(tail));
                        payload = BuildEventsJson(batch);
                    }

                    if (ForwardFrameBytes(payload.size()) > kMaxFrameBytes) {
                        // Одна подія більша за кадр — за нинішніх меж тексту так
                        // не буває. Якщо все ж сталося, відкидаємо саме її,
                        // а не зупиняємо всю доставку.
                        Log().Warnf("подію відкинуто: завелика для кадру (%zu байтів)", payload.size());
                    } else {
                        std::vector<std::string> frames;
                        {
                            Guard guard(linksLock_);
                            for (PhoneLink& link : links_) {
                                if (!link.online || link.wantsSnapshot) continue;
                                std::string frame = EncryptToForwardFrame(
                                    link.key, link.counter, payload, link.phone.deviceId);
                                if (!frame.empty()) frames.push_back(std::move(frame));
                            }
                        }

                        size_t delivered = 0;
                        for (const std::string& frame : frames) {
                            if (!socket.SendText(frame)) break;
                            ++delivered;
                        }

                        if (delivered == 0) {
                            // Відправлення не вдалося — події повертаються в чергу,
                            // щоб не загубитися. Пріоритети зберігаються.
                            queue_.Requeue(std::move(batch));
                        } else {
                            lastDataMs = NowMonotonicMs();
                            Log().Debugf("надіслано %zu подій на %zu телефонів", batch.size(), delivered);
                        }
                    }
                }
            }
        }

        const uint64_t now = NowMonotonicMs();

        // Періодичне оновлення стану, навіть коли подій немає.
        //
        // Без нього застосунок, не отримуючи нічого, за пів хвилини вважав
        // би дані застарілими — хоча зв'язок цілий, а Claude Code просто
        // нічого не робить. Заразом оновлюються час роботи задач і
        // споживання ресурсів Bridge.
        if (now - lastDataMs >= kIdleRefreshMs) {
            snapshotRequested_ = true;
            lastDataMs = now;
        }

        if (now - lastPingMs >= kPingIntervalMs) {
            socket.SendText(MakePingFrame());
            lastPingMs = now;
        }
    }

    // Зупиняємо читача. Він може стояти в блокуючому Receive, тому
    // спершу закриваємо сокет — це змушує виклик повернутися.
    readerActive = false;
    socket.Abort();

    if (::WaitForSingleObject(reader.get(), 3000) == WAIT_TIMEOUT) {
        Log().Warn("потік читання не завершився вчасно");
    }
}

void BridgeApp::NetworkLoop() {
    Backoff backoff;

    while (networkRunning_) {
        if (!HasPhones()) {
            // Немає спареного телефона — підключатися немає сенсу. Чекаємо на
            // сигнал завершення, заразом стежачи, чи не спарили телефон.
            ::WaitForSingleObject(networkWakeEvent_.get(), 5000);
            // Подія ручна: без скидання очікування одразу поверталося б знову.
            if (networkRunning_) ::ResetEvent(networkWakeEvent_.get());
            if (FileWriteStamp(config_.peerPath()) != phonesStamp_) ReloadPhones();
            continue;
        }

        state_ = backoff.attempts() == 0 ? BridgeState::Connecting
                                         : BridgeState::Reconnecting;

        WebSocketClient socket;
        if (ConnectAndAuthenticate(socket)) {
            // Лічильник скидається саме після УСПІШНОЇ автентифікації,
            // а не після встановлення з'єднання: інакше цикл
            // «підключився — відмовили» перетворився б на потік спроб
            // (Частина 5 §20 Master Prompt).
            backoff.Reset();
            state_ = BridgeState::Monitoring;
            snapshotRequested_ = true;

            ServeConnection(socket);

            socket.Close(1001, "reconnect");
            Log().Info("з'єднання з Relay втрачено");
        } else {
            socket.Abort();
        }

        if (!networkRunning_) break;

        const uint32_t delay = backoff.NextDelayMs();
        Log().Debugf("повторна спроба підключення через %u мс", delay);

        // Очікування переривається сигналом завершення, тож зупинка
        // Bridge не змушує чекати повний інтервал.
        ::WaitForSingleObject(networkWakeEvent_.get(), delay);
        ::ResetEvent(networkWakeEvent_.get());
    }

    Log().Debug("мережевий потік завершено");
}

int BridgeApp::Run() {
    startedAtMs_ = NowMonotonicMs();
    state_ = BridgeState::Detecting;

    // ── Прив'язка до Claude Desktop ──────────────────────────────────────
    const ClaudeDesktopInfo desktop = DesktopMonitor::Find();
    if (desktop.found) {
        desktopPid_ = desktop.pid;
        desktopProcess_ = DesktopMonitor::OpenForWait(desktop.pid);
        Log().Infof("знайдено Claude Desktop, PID %u", desktop.pid);
    } else {
        // Bridge усе одно працює: сесії Claude Code можуть існувати й без
        // виявленого кореневого процесу. Просто немає на що чекати.
        Log().Warn("процес Claude Desktop не знайдено, працюю без прив'язки до нього");
    }

    // ── Телефони й ключі наскрізного шифрування ──────────────────────────
    phonesStamp_ = FileWriteStamp(config_.peerPath());
    {
        Guard guard(linksLock_);
        RebuildLinksLocked();
        if (links_.empty()) {
            state_ = BridgeState::Unpaired;
            Log().Warn("жодного телефона не підключено: запустіть з --pair");
        } else {
            Log().Infof("підключено телефонів: %zu з %zu", links_.size(), kMaxPairedPhones);
        }
    }

    // ── Адреса Relay ─────────────────────────────────────────────────────
    if (config_.relayUrl.empty()) {
        Log().Error("адресу Relay не налаштовано: запустіть з --set-relay <url>");
        return 2;
    }
    if (!RelayEndpoint::Parse(config_.relayUrl, endpoint_, config_.allowInsecure)) {
        Log().Error("некоректна адреса Relay");
        return 2;
    }

    // ── Спостереження ────────────────────────────────────────────────────
    watch_.Start(scanner_.sessionsDir(), scanner_.projectsDir());
    if (desktopProcess_) watch_.AddExternalHandle(desktopProcess_.get());

    hookServer_.Start([this](const HookMessage& message) { HandleHookMessage(message); });

    // Вузли підключаються останніми: без них Bridge працює так само.
    nodes_.Start();
    if (nodes_.count() > 0) Log().Infof("вузлів підключено: %zu", nodes_.count());

    RescanSessions();
    Log().Infof("Bridge працює, задач під наглядом: %zu", manager_.sessions().size());

    // ── Мережевий потік ──────────────────────────────────────────────────
    networkWakeEvent_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    networkRunning_ = true;

    RefreshSnapshotPayload();

    networkThread_.reset(::CreateThread(nullptr, 0, &BridgeApp::NetworkThreadEntry,
                                        this, 0, nullptr));
    if (!networkThread_) {
        Log().Error("не вдалося створити мережевий потік");
        networkRunning_ = false;
    }

    // ── Головний цикл спостереження ──────────────────────────────────────
    //
    // Блокується на об'єктах ядра і прокидається лише від подій файлової
    // системи. У стані спокою процесорний час не витрачається взагалі
    // (Частина 5 §31, §32 Master Prompt).
    //
    // Мережі тут немає жодної: усе, що цей потік робить із подіями, —
    // кладе їх у чергу.
    uint64_t lastRescanMs = NowMonotonicMs();
    uint64_t lastSnapshotMs = NowMonotonicMs();
    uint64_t lastPollMs = NowMonotonicMs();
    uint64_t lastDesktopCheckMs = NowMonotonicMs();
    uint64_t desktopGoneSinceMs = 0;

    while (!g_shutdownRequested) {
        const WatchEvent event = watch_.Wait(kMainLoopTimeoutMs);

        if (event == WatchEvent::Shutdown) {
            // Сигнал зсередини (Ctrl+C, зупинка служби) — виходимо одразу.
            if (!watch_.externalSignalled()) break;

            // Закрилося вікно Claude Desktop. Це ще не привід зупинятися:
            // сесії Claude Code живуть і без нього, а телефон інакше лишався б
            // із застиглим чатом, хоча Claude продовжує працювати.
            watch_.ClearExternalHandle();
            desktopProcess_.reset();
            desktopPid_ = 0;

            const ClaudeDesktopInfo restarted = DesktopMonitor::Find();
            if (restarted.found) {
                desktopPid_ = restarted.pid;
                desktopProcess_ = DesktopMonitor::OpenForWait(restarted.pid);
                if (desktopProcess_) watch_.AddExternalHandle(desktopProcess_.get());
                Log().Infof("Claude Desktop перезапущено, PID %u", restarted.pid);
                continue;
            }

            RescanSessions();
            if (manager_.sessions().empty()) {
                Log().Info("Claude Desktop завершився, активних задач немає — зупиняю Bridge");
                break;
            }

            desktopGoneSinceMs = NowMonotonicMs();
            Log().Infof("Claude Desktop завершився, але задач під наглядом %zu — працюю далі",
                        manager_.sessions().size());
            continue;
        }

        // Канал хуків перевіряється щоразу: він дешевий і не блокує.
        hookServer_.Consume();

        bool stateChanged = false;

        switch (event) {
            case WatchEvent::SessionsChanged:
                RescanSessions();
                lastRescanMs = NowMonotonicMs();
                stateChanged = true;
                break;

            case WatchEvent::TranscriptChanged:
                stateChanged = manager_.PollTranscripts() > 0;
                lastPollMs = NowMonotonicMs();
                break;

            case WatchEvent::Timeout: {
                const uint64_t now = NowMonotonicMs();

                // Claude Desktop могли запустити знову — тоді Bridge знову
                // завершиться разом із ним. Якщо ж немає ні його, ні задач,
                // стежити нема за чим.
                if (!desktopProcess_ && now - lastDesktopCheckMs > kDesktopCheckMs) {
                    lastDesktopCheckMs = now;
                    const ClaudeDesktopInfo again = DesktopMonitor::Find();
                    if (again.found) {
                        desktopPid_ = again.pid;
                        desktopProcess_ = DesktopMonitor::OpenForWait(again.pid);
                        if (desktopProcess_) watch_.AddExternalHandle(desktopProcess_.get());
                        desktopGoneSinceMs = 0;
                        Log().Infof("знайдено Claude Desktop, PID %u", again.pid);
                    } else if (desktopGoneSinceMs != 0 && manager_.sessions().empty() &&
                               now - desktopGoneSinceMs > kOrphanGraceMs) {
                        Log().Info("Claude Desktop немає, задач теж — зупиняю Bridge");
                        g_shutdownRequested = true;
                    }
                }

                // Запобіжна перевірка реєстру: повідомлення файлової системи
                // зрідка губляться, і без цього нова задача могла б лишитися
                // непоміченою.
                if (now - lastRescanMs > 30'000) {
                    RescanSessions();
                    lastRescanMs = now;
                    stateChanged = true;
                }
                break;
            }

            default:
                break;
        }

        // Дочитування транскриптів — незалежно від того, що розбудило цикл.
        //
        // Раніше воно жило лише в гілці тайм-ауту, а тайм-аут не настає, поки
        // приходять інші події: Claude Code постійно оновлює реєстр сесій.
        // Разом із відкладеними повідомленнями Windows про сам транскрипт це
        // давало хвилини, протягом яких написана відповідь не доходила до телефона.
        if (NowMonotonicMs() - lastPollMs >= kTranscriptPollMs) {
            if (manager_.PollTranscripts() > 0) stateChanged = true;
            lastPollMs = NowMonotonicMs();

            // Ліміт, час скидання якого минув, знімається й без нових записів
            // у транскрипті. Знімок оновлюємо й надсилаємо одразу, не чекаючи
            // планового оновлення: на телефоні стан задачі має змінитися вчасно.
            if (manager_.ReleaseExpiredLimits(NowUnixMs())) {
                RefreshSnapshotPayload();
                lastSnapshotMs = NowMonotonicMs();
                snapshotRequested_ = true;
            }
        }

        // Знімок оновлюється не частіше, ніж раз на секунду: він потрібен
        // лише при підключенні телефона, а перерахунок при кожній події
        // був би марною роботою.
        const uint64_t now = NowMonotonicMs();
        if ((stateChanged && now - lastSnapshotMs > 1000) || snapshotRequested_) {
            RefreshSnapshotPayload();
            lastSnapshotMs = now;
        }
    }

    // ── Коректне завершення ──────────────────────────────────────────────
    state_ = BridgeState::ShuttingDown;
    Log().Info("завершення роботи");

    nodes_.Stop();
    hookServer_.Stop();
    watch_.Shutdown();

    // Зупинка мережевого потоку з обмеженням часу: завершення Bridge
    // не має зависати через мережу (Частина 5 §36 Master Prompt).
    networkRunning_ = false;
    if (networkWakeEvent_) ::SetEvent(networkWakeEvent_.get());

    if (networkThread_) {
        if (::WaitForSingleObject(networkThread_.get(), 3000) == WAIT_TIMEOUT) {
            Log().Warn("мережевий потік не завершився вчасно");
        }
    }

    const QueueStats stats = queue_.stats();
    Log().Infof("статистика черги: додано %llu, надіслано %llu, відкинуто %llu",
                static_cast<unsigned long long>(stats.pushed),
                static_cast<unsigned long long>(stats.popped),
                static_cast<unsigned long long>(stats.droppedLow + stats.droppedMedium));

    state_ = BridgeState::Stopped;
    return 0;
}

// ── Режими командного рядка ──────────────────────────────────────────────────

namespace {

int RunHookMode(std::string_view eventName) {
    // Найкоротший можливий шлях: прочитати вхід, передати в канал, вийти.
    // Claude Code чекає завершення цього процесу, тому тут не можна робити
    // нічого повільного — ані мережі, ані розбору транскриптів.
    const std::string input = ReadStdinAll();

    HookMessage message;
    ParseHookInput(input, eventName, message);

    // Якщо робочого Bridge немає, повідомлення просто нікуди не піде.
    // Це не помилка: Claude Code не повинен постраждати від того,
    // що моніторинг вимкнений.
    SendHookMessage(message);
    return 0;
}

void PrintHookInstructions() {
    wchar_t exePath[MAX_PATH * 2]{};
    ::GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));

    std::string path = Utf16ToUtf8(exePath);
    // Подвоюємо зворотні скісні риски для JSON.
    std::string escaped;
    for (char c : path) {
        if (c == '\\') escaped += "\\\\";
        else escaped += c;
    }

    std::printf(
        "Додайте цей фрагмент у файл %%USERPROFILE%%\\.claude\\settings.json\n"
        "у розділ \"hooks\" (якщо розділу немає — створіть):\n\n"
        "{\n"
        "  \"hooks\": {\n"
        "    \"SessionStart\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\", \"command\": \"\\\"%s\\\" --hook session-start\" } ] }\n"
        "    ],\n"
        "    \"SessionEnd\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\", \"command\": \"\\\"%s\\\" --hook session-end\" } ] }\n"
        "    ],\n"
        "    \"Notification\": [\n"
        "      { \"hooks\": [ { \"type\": \"command\", \"command\": \"\\\"%s\\\" --hook notification\" } ] }\n"
        "    ]\n"
        "  }\n"
        "}\n\n"
        "Використовуються лише три хуки, і всі вони спрацьовують рідко.\n"
        "Хуки на кожен виклик інструмента свідомо не застосовуються:\n"
        "ті самі дані вже є в транскрипті, а створення процесу на кожну дію\n"
        "уповільнювало б Claude Code.\n",
        escaped.c_str(), escaped.c_str(), escaped.c_str());
}

/// Дата у звичному вигляді — щоб телефони в переліку можна було розрізнити.
std::string FormatLocalDate(uint64_t unixMs) {
    if (unixMs == 0) return "дата невідома";

    ULARGE_INTEGER value{};
    value.QuadPart = unixMs * 10000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;

    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!::FileTimeToSystemTime(&ft, &utc) ||
        !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return "дата невідома";
    }

    char text[32]{};
    std::snprintf(text, sizeof(text), "%02u.%02u.%04u %02u:%02u",
                  local.wDay, local.wMonth, local.wYear, local.wHour, local.wMinute);
    return text;
}

/// Перелік спарених телефонів — для --status, --pair і --unpair.
void PrintPhones(const Identity& identity) {
    if (identity.phones.empty()) {
        std::printf("    телефонів немає\n");
        return;
    }
    for (size_t i = 0; i < identity.phones.size(); ++i) {
        const PairedPhone& phone = identity.phones[i];
        std::printf("    %zu. %s  підключено %s\n", i + 1,
                    phone.deviceId.substr(0, 16).c_str(),
                    FormatLocalDate(phone.pairedAtMs).c_str());
    }
}

int RunStatusMode(const Config& config, Identity& identity) {
    std::printf("Claude Monitor Bridge %s\n\n", kBridgeVersionA);

    const ClaudeDesktopInfo desktop = DesktopMonitor::Find();
    std::printf("Claude Desktop:  %s\n",
                desktop.found ? "працює" : "не знайдено");
    if (desktop.found) std::printf("  PID:           %u\n", desktop.pid);

    SingleInstance instance;
    const bool acquired = instance.Acquire();
    std::printf("Робочий Bridge:  %s\n", acquired ? "не запущений" : "працює");

    std::printf("\nІдентичність:\n");
    if (identity.deviceId.empty()) {
        std::printf("  ще не створена\n");
    } else {
        std::printf("  пристрій:      %s\n", identity.deviceId.substr(0, 16).c_str());
        std::printf("  телефонів:     %zu з %zu\n", identity.phones.size(), kMaxPairedPhones);
        PrintPhones(identity);
    }

    std::printf("\nRelay:           %s\n",
                config.relayUrl.empty() ? "не налаштовано"
                                        : Utf16ToUtf8(config.relayUrl).c_str());

    SessionScanner scanner;
    const auto sessions = scanner.Scan();
    std::printf("\nАктивних задач Claude Code: %zu\n", sessions.size());
    for (const auto& session : sessions) {
        std::printf("  %s  PID %-6u  %s\n",
                    session.sessionId.substr(0, 8).c_str(),
                    session.pid,
                    session.cwd.c_str());
    }

    std::printf("\nКаталог даних:   %s\n", Utf16ToUtf8(config.dataDir).c_str());
    return 0;
}

/// Режим pairing: показує код і чекає, доки телефон його введе.
int RunPairMode(Config& config, Identity& identity) {
    if (config.relayUrl.empty()) {
        std::printf("Спершу налаштуйте адресу Relay:\n");
        std::printf("  claude-monitor-bridge --set-relay wss://ваш-домен/ws\n");
        return 2;
    }

    // Межа — до показу коду: інакше користувач ввів би код даремно.
    if (identity.phones.size() >= kMaxPairedPhones) {
        std::printf("Уже підключено %zu телефонів — це максимум.\n", identity.phones.size());
        std::printf("Щоб підключити ще один, спершу від'єднайте котрийсь:\n\n");
        PrintPhones(identity);
        std::printf("\n  claude-monitor-bridge --unpair <перші символи ідентифікатора>\n");
        return 3;
    }

    RelayEndpoint endpoint;
    if (!RelayEndpoint::Parse(config.relayUrl, endpoint, config.allowInsecure)) {
        std::printf("Некоректна адреса Relay: %s\n", Utf16ToUtf8(config.relayUrl).c_str());
        return 2;
    }

    PairingSession pairing;
    if (!pairing.Begin(identity.signing, identity.exchange)) {
        std::printf("Не вдалося підготувати pairing.\n");
        return 1;
    }

    WebSocketClient socket;
    if (!socket.Connect(endpoint)) {
        std::printf("Не вдалося підключитися до Relay: %s\n", socket.lastError().c_str());
        return 1;
    }

    if (!socket.SendText(MakePairOfferFrame(pairing.offerId(),
                                            pairing.publicSign(),
                                            pairing.publicExchange(),
                                            pairing.bridgeConfirm()))) {
        std::printf("Не вдалося надіслати пропозицію pairing.\n");
        return 1;
    }

    std::printf("\n");
    std::printf("  ┌────────────────────────────────────┐\n");
    std::printf("  │   КОД ДЛЯ ПІДКЛЮЧЕННЯ ТЕЛЕФОНА     │\n");
    std::printf("  │                                    │\n");
    std::printf("  │          %s          │\n", pairing.formattedCode().c_str());
    std::printf("  │                                    │\n");
    std::printf("  └────────────────────────────────────┘\n\n");
    std::printf("Введіть цей код у застосунку Claude Monitor на телефоні.\n");
    std::printf("Код дійсний %u секунд і використовується один раз.\n", kPairingTtlSec);
    std::printf("Уже підключено телефонів: %zu з %zu.\n\n", identity.phones.size(), kMaxPairedPhones);
    std::printf("Очікування…\n");

    // Без цього код не з'явиться, якщо вивід перенаправлено у файл або
    // канал: стандартний потік у такому разі буферизується, а процес
    // продовжує чекати на телефон і нічого не друкує.
    std::fflush(stdout);

    const uint64_t deadline = NowMonotonicMs() + static_cast<uint64_t>(kPairingTtlSec) * 1000;

    while (NowMonotonicMs() < deadline && !g_shutdownRequested) {
        std::string raw;
        if (!socket.Receive(raw, 1000)) {
            if (!socket.connected()) {
                std::printf("\nЗ'єднання з Relay втрачено.\n");
                return 1;
            }
            continue;
        }

        const IncomingFrame frame = ParseIncomingFrame(raw, NowUnixMs());
        if (!frame.valid) continue;

        if (frame.type == "error") {
            std::printf("\nRelay повідомив про помилку: %s\n",
                        frame.payload["code"].asStringOr("").c_str());
            continue;
        }

        if (frame.type != "pair_claim") continue;

        const std::string peerSign = frame.payload["pub_sig"].asStringOr("");
        const std::string peerExchange = frame.payload["pub_ecdh"].asStringOr("");
        const std::string peerConfirm = frame.payload["confirm"].asStringOr("");

        // Ключова перевірка: підтвердити володіння кодом може лише той,
        // хто його бачив. Relay коду не знає, тож підмінити ключі не може.
        if (!pairing.VerifyPeer(peerSign, peerExchange, peerConfirm)) {
            std::printf("\nНевірний код. Спробу відхилено.\n");
            socket.SendText(MakePairOkFrame(pairing.offerId(), false));
            continue;
        }

        crypto::Sha256Digest digest{};
        std::vector<uint8_t> peerPoint;
        Base64UrlDecode(peerSign, peerPoint);
        crypto::Sha256(peerPoint.data(), peerPoint.size(), digest);

        PairedPhone phone;
        phone.deviceId = HexEncode(digest.data(), digest.size());
        phone.publicSign = peerSign;
        phone.publicExchange = peerExchange;
        phone.pairedAtMs = NowUnixMs();

        // Поки чекали на телефон, перелік міг змінити інший процес — перечитуємо.
        identity.LoadPhones(config);
        if (!identity.AddPhone(phone)) {
            std::printf("\nУже підключено %zu телефонів — це максимум. Спробу відхилено.\n",
                        identity.phones.size());
            socket.SendText(MakePairOkFrame(pairing.offerId(), false));
            socket.Close();
            return 3;
        }

        if (!identity.SavePhones(config)) {
            std::printf("\nНе вдалося зберегти дані пристрою.\n");
            return 1;
        }

        socket.SendText(MakePairOkFrame(pairing.offerId(), true));

        std::printf("\n  Телефон підключено.\n");
        std::printf("  Ідентифікатор: %s…\n", phone.deviceId.substr(0, 16).c_str());
        std::printf("  Підключено телефонів: %zu з %zu.\n\n", identity.phones.size(), kMaxPairedPhones);

        pairing.Clear();
        socket.Close();
        return 0;
    }

    std::printf("\nЧас очікування вичерпано. Код більше не дійсний.\n");
    socket.Close();
    return 1;
}

/// Від'єднання телефона. Без ідентифікатора — усі телефони.
int RunUnpairMode(const Config& config, Identity& identity, const std::wstring& idArg) {
    if (idArg.empty()) {
        const size_t count = identity.phones.size();
        identity.ForgetAllPhones();
        if (!identity.SavePhones(config)) {
            std::printf("Не вдалося зберегти перелік телефонів.\n");
            return 1;
        }
        std::printf("Від'єднано телефонів: %zu. Для підключення потрібен новий pairing.\n", count);
        return 0;
    }

    std::string prefix = Utf16ToUtf8(idArg);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });

    // Короткий префікс легко збігся б не з тим телефоном.
    if (prefix.size() < 4) {
        std::printf("Вкажіть щонайменше 4 перші символи ідентифікатора телефона:\n\n");
        PrintPhones(identity);
        return 2;
    }

    const size_t matches = static_cast<size_t>(std::count_if(
        identity.phones.begin(), identity.phones.end(), [&prefix](const PairedPhone& phone) {
            return phone.deviceId.compare(0, prefix.size(), prefix) == 0;
        }));

    if (matches == 0) {
        std::printf("Телефона з ідентифікатором %s… не знайдено. Підключені телефони:\n\n",
                    prefix.c_str());
        PrintPhones(identity);
        return 2;
    }
    if (matches > 1) {
        std::printf("Під %s… підходить кілька телефонів — вкажіть більше символів:\n\n",
                    prefix.c_str());
        PrintPhones(identity);
        return 2;
    }

    identity.RemovePhones(prefix);
    if (!identity.SavePhones(config)) {
        std::printf("Не вдалося зберегти перелік телефонів.\n");
        return 1;
    }

    std::printf("Телефон %s… від'єднано. Лишилось телефонів: %zu з %zu.\n",
                prefix.c_str(), identity.phones.size(), kMaxPairedPhones);
    return 0;
}

void PrintUsage() {
    std::printf(
        "Claude Monitor Bridge %s\n\n"
        "Використання:\n"
        "  claude-monitor-bridge                 робочий режим\n"
        "  claude-monitor-bridge --pair          підключити телефон (до %zu телефонів)\n"
        "  claude-monitor-bridge --unpair [ID]   від'єднати телефон; без ID — усі\n"
        "  claude-monitor-bridge --status        стан, підключені телефони, діагностика\n"
        "  claude-monitor-bridge --set-relay URL зберегти адресу Relay\n"
        "  claude-monitor-bridge --install-hooks показати налаштування хуків\n"
        "  claude-monitor-bridge --hook ПОДІЯ    службовий виклик від Claude Code\n\n"
        "Додаткові прапорці:\n"
        "  --log-level error|warn|info|debug\n"
        "  --console                             дублювати журнал у консоль\n"
        "  --allow-insecure                      дозволити ws:// (лише розробка)\n",
        kBridgeVersionA, kMaxPairedPhones);
}

}  // namespace
}  // namespace cm

// ── Точка входу ──────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t** argv) {
    using namespace cm;

    EnableUtf8Console();
    ::SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // Розбір аргументів.
    std::wstring command;
    std::wstring hookEvent;
    std::wstring relayArg;
    std::wstring unpairArg;
    LogLevel logLevel = LogLevel::Info;
    bool console = false;
    bool allowInsecure = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];

        if (arg == L"--hook" && i + 1 < argc) { command = L"hook"; hookEvent = argv[++i]; }
        else if (arg == L"--pair")            { command = L"pair"; }
        else if (arg == L"--unpair") {
            command = L"unpair";
            // Необов'язковий ідентифікатор телефона одразу після прапорця.
            if (i + 1 < argc && argv[i + 1][0] != L'-') unpairArg = argv[++i];
        }
        else if (arg == L"--status")          { command = L"status"; }
        else if (arg == L"--install-hooks")   { command = L"install-hooks"; }
        else if (arg == L"--set-relay" && i + 1 < argc) { command = L"set-relay"; relayArg = argv[++i]; }
        else if (arg == L"--console")         { console = true; }
        else if (arg == L"--allow-insecure")  { allowInsecure = true; }
        else if (arg == L"--log-level" && i + 1 < argc) {
            logLevel = ParseLogLevel(Utf16ToUtf8(argv[++i]));
        }
        else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            return 0;
        }
    }

    // Режим хука обробляється до будь-якої іншої ініціалізації: він має
    // завершитися якнайшвидше, бо Claude Code чекає саме на нього.
    if (command == L"hook") {
        return RunHookMode(Utf16ToUtf8(hookEvent));
    }

    Config config;
    config.dataDir = BridgeDataDir();
    config.allowInsecure = allowInsecure;
    config.Load();
    if (allowInsecure) config.allowInsecure = true;
    if (logLevel != LogLevel::Info) config.logLevel = logLevel;

    // Для інтерактивних команд журнал дублюється в консоль; у робочому
    // режимі за замовчуванням пишеться лише у файл.
    const bool interactive = !command.empty() && command != L"install-hooks";
    Log().Configure(config.logPath, config.logLevel, console || interactive);

    if (command == L"install-hooks") {
        PrintHookInstructions();
        return 0;
    }

    if (command == L"set-relay") {
        config.relayUrl = relayArg;
        if (!config.Save()) {
            std::printf("Не вдалося зберегти конфігурацію.\n");
            return 1;
        }
        std::printf("Адресу Relay збережено: %s\n", Utf16ToUtf8(relayArg).c_str());
        return 0;
    }

    // Ідентичність потрібна всім іншим режимам.
    Identity identity;
    if (!identity.Load(config)) {
        if (!identity.Create(config)) {
            std::printf("Не вдалося створити криптографічну ідентичність.\n");
            return 1;
        }
    }

    if (command == L"status")  return RunStatusMode(config, identity);
    if (command == L"pair")    return RunPairMode(config, identity);
    if (command == L"unpair")  return RunUnpairMode(config, identity, unpairArg);

    // ── Робочий режим ────────────────────────────────────────────────────
    //
    // Єдиний екземпляр: якщо Bridge уже працює, другий процес просто
    // виходить (Частина 5 §8 Master Prompt).
    SingleInstance instance;
    if (!instance.Acquire()) {
        if (instance.alreadyRunning()) {
            Log().Debug("Bridge уже працює, завершуюсь");
            return 0;
        }
        Log().Error("не вдалося перевірити єдиність екземпляра");
        return 1;
    }

    Log().Infof("Bridge %s запускається", kBridgeVersionA);

    BridgeApp app(config, identity);
    const int result = app.Run();

    Log().Info("Bridge зупинено");
    return result;
}
