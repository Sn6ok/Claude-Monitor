// watcher.cpp — спостереження за каталогами через ReadDirectoryChangesW.

#include "watcher.h"
#include "logging.h"

namespace cm {

namespace {

/// Розмір буфера повідомлень ядра. Достатній, щоб вмістити типову серію
/// змін; при переповненні ядро повідомляє про це, і Bridge просто
/// перечитує файли повністю — тому втрата деталей нічого не ламає.
constexpr DWORD kNotifyBufferSize = 16 * 1024;

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME |
    FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_SIZE;

}  // namespace

// ── DirectoryWatcher ─────────────────────────────────────────────────────────

DirectoryWatcher::~DirectoryWatcher() { Stop(); }

bool DirectoryWatcher::Start(const std::wstring& path, bool recursive) {
    Stop();

    path_ = path;
    recursive_ = recursive;
    buffer_.resize(kNotifyBufferSize);

    // FILE_FLAG_BACKUP_SEMANTICS обов'язковий для відкриття каталогу.
    // FILE_FLAG_OVERLAPPED дає асинхронну роботу — саме вона дозволяє
    // чекати на об'єкті ядра замість опитування.
    directory_.reset(::CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr));

    if (!directory_) {
        Log().Warnf("не вдалося відкрити каталог для спостереження (код %lu)", ::GetLastError());
        return false;
    }

    event_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event_) { directory_.reset(); return false; }

    overlapped_ = {};
    overlapped_.hEvent = event_.get();

    return Queue();
}

bool DirectoryWatcher::Queue() {
    if (!directory_) return false;

    ::ResetEvent(overlapped_.hEvent);

    const BOOL ok = ::ReadDirectoryChangesW(
        directory_.get(),
        buffer_.data(),
        static_cast<DWORD>(buffer_.size()),
        recursive_ ? TRUE : FALSE,
        kNotifyFilter,
        nullptr,
        &overlapped_,
        nullptr);

    if (!ok) {
        Log().Warnf("ReadDirectoryChangesW не вдався (код %lu)", ::GetLastError());
        return false;
    }
    return true;
}

bool DirectoryWatcher::Consume() {
    if (!directory_) return false;

    DWORD transferred = 0;
    // FALSE у останньому параметрі: не чекати. Подія вже сигналізована,
    // тож результат готовий.
    if (!::GetOverlappedResult(directory_.get(), &overlapped_, &transferred, FALSE)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_IO_INCOMPLETE) return true;  // хибне пробудження

        Log().Warnf("GetOverlappedResult не вдався (код %lu)", error);
        return Queue();
    }

    // transferred == 0 означає переповнення буфера ядра: перелік змін
    // втрачено. Це не проблема, бо Bridge усе одно перечитує файли
    // з відомих зміщень, а не покладається на імена з повідомлення.
    if (transferred == 0) {
        Log().Debug("буфер повідомлень файлової системи переповнено, перечитую повністю");
    }

    return Queue();
}

void DirectoryWatcher::Stop() {
    if (directory_) {
        ::CancelIoEx(directory_.get(), &overlapped_);
        directory_.reset();
    }
    event_.reset();
    overlapped_ = {};
    buffer_.clear();
    buffer_.shrink_to_fit();
}

// ── WatchSet ─────────────────────────────────────────────────────────────────

WatchSet::WatchSet() {
    // Подія з ручним скиданням: сигнал завершення має побачити кожен,
    // хто чекає, а не лише перший потік.
    shutdownEvent_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
}

WatchSet::~WatchSet() = default;

bool WatchSet::Start(const std::wstring& sessionsDir, const std::wstring& projectsDir) {
    bool any = false;

    if (!sessionsDir.empty() && sessions_.Start(sessionsDir, false)) {
        Log().Debug("спостерігаю за каталогом сесій");
        any = true;
    }
    // Транскрипти лежать у підкаталогах за проєктами, тому рекурсивно.
    if (!projectsDir.empty() && projects_.Start(projectsDir, true)) {
        Log().Debug("спостерігаю за каталогом транскриптів");
        any = true;
    }

    if (!any) {
        // Bridge продовжить працювати на самому лише запобіжному таймері.
        // Це гірше за подієву модель, але краще за відмову: користувач
        // усе одно бачитиме стан, хай і з затримкою.
        Log().Warn("спостереження за файлами недоступне, працюю за таймером");
    }
    return any;
}

void WatchSet::AddExternalHandle(HANDLE handle) {
    external_ = handle;
    externalSignalled_ = false;
}

void WatchSet::ClearExternalHandle() {
    external_ = nullptr;
    externalSignalled_ = false;
}

void WatchSet::Shutdown() {
    if (shutdownEvent_) ::SetEvent(shutdownEvent_.get());
}

WatchEvent WatchSet::Wait(uint32_t timeoutMs) {
    HANDLE handles[4];
    WatchEvent kinds[4];
    DWORD count = 0;

    // Порядок важливий: WaitForMultipleObjects за рівних умов повертає
    // найменший індекс, тож завершення має стояти першим.
    if (shutdownEvent_) {
        handles[count] = shutdownEvent_.get();
        kinds[count] = WatchEvent::Shutdown;
        ++count;
    }
    if (external_) {
        handles[count] = external_;
        kinds[count] = WatchEvent::Shutdown;
        ++count;
    }
    if (sessions_.active()) {
        handles[count] = sessions_.waitHandle();
        kinds[count] = WatchEvent::SessionsChanged;
        ++count;
    }
    if (projects_.active()) {
        handles[count] = projects_.waitHandle();
        kinds[count] = WatchEvent::TranscriptChanged;
        ++count;
    }

    if (count == 0) {
        ::Sleep(timeoutMs);
        return WatchEvent::Timeout;
    }

    const DWORD result = ::WaitForMultipleObjects(count, handles, FALSE, timeoutMs);

    if (result == WAIT_TIMEOUT) return WatchEvent::Timeout;
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
        const DWORD index = result - WAIT_OBJECT_0;

        // Зовнішній дескриптор — це процес Claude Desktop. Його сигналізація
        // означає, що Claude Desktop завершився і Bridge має піти слідом.
        if (external_ && handles[index] == external_) {
            externalSignalled_ = true;
            return WatchEvent::Shutdown;
        }

        if (kinds[index] == WatchEvent::SessionsChanged) sessions_.Consume();
        else if (kinds[index] == WatchEvent::TranscriptChanged) projects_.Consume();

        return kinds[index];
    }

    if (result == WAIT_FAILED) {
        Log().Errorf("WaitForMultipleObjects не вдався (код %lu)", ::GetLastError());
        // Невелика пауза, щоб помилка не перетворилася на цикл із повним
        // завантаженням процесора (Частина 5 §32 Master Prompt).
        ::Sleep(1000);
    }
    return WatchEvent::Timeout;
}

}  // namespace cm
