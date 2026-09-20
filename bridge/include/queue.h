// queue.h — обмежена черга подій між спостереженням і мережею.
//
// Це той самий буфер, який робить архітектуру безпечною для Claude Code:
// потік спостереження ніколи не чекає на мережу, бо кладе подію в чергу
// і одразу повертається. Затримка чи обрив зв'язку фізично не можуть
// дійти до читача транскрипту (Частина 5 §15, §19 Master Prompt).
//
// Черга ОБМЕЖЕНА за кількістю подій і за обсягом пам'яті. При переповненні
// витісняються найменш важливі події, а не найновіші: користувач має
// побачити, що задача завершилась або впала, навіть якщо частину проміжного
// виводу буде втрачено (docs/protocol.md §8).

#pragma once

#include "events.h"

#include <deque>

namespace cm {

struct QueueStats {
    uint64_t pushed = 0;
    uint64_t popped = 0;
    uint64_t droppedLow = 0;
    uint64_t droppedMedium = 0;
    uint64_t droppedOversize = 0;
    size_t   currentCount = 0;
    size_t   currentBytes = 0;
    size_t   peakCount = 0;
};

class EventQueue {
public:
    /// @param maxEvents максимальна кількість подій
    /// @param maxBytes  максимальний сумарний обсяг корисних даних
    explicit EventQueue(size_t maxEvents = kMaxQueueEvents,
                        size_t maxBytes = 2u * 1024 * 1024);

    /// Кладе подію в чергу. Ніколи не блокує і завжди повертає керування.
    /// @returns false, якщо подію довелося відкинути повністю
    bool Push(Event&& event);

    /// Забирає до `maxCount` подій у порядку надходження.
    size_t Drain(std::vector<Event>& out, size_t maxCount);

    /// Повертає невідправлені події назад у чергу — застосовується, коли
    /// відправлення не вдалося. Порядок і пріоритети зберігаються.
    void Requeue(std::vector<Event>&& events);

    void Clear();

    bool   empty() const;
    size_t size() const;
    QueueStats stats() const;

private:
    /// Звільняє місце під нову подію, витісняючи найменш важливі.
    /// @returns true, якщо місця вистачає після витіснення
    bool MakeRoom(size_t incomingBytes, Priority incomingPriority);

    mutable Lock       lock_;
    std::deque<Event>  events_;
    size_t             maxEvents_;
    size_t             maxBytes_;
    size_t             currentBytes_ = 0;
    QueueStats         stats_;
};

}  // namespace cm
