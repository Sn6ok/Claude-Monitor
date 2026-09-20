// queue.cpp — реалізація обмеженої черги подій.

#include "queue.h"

#include <algorithm>

namespace cm {

EventQueue::EventQueue(size_t maxEvents, size_t maxBytes)
    : maxEvents_(maxEvents), maxBytes_(maxBytes) {}

bool EventQueue::MakeRoom(size_t incomingBytes, Priority incomingPriority) {
    // Витіснення йде знизу вгору за пріоритетом: спершу Low, потім Medium.
    // High не витісняється ніколи — це гарантія, що зміни стану задач
    // і помилки дійдуть до користувача.
    for (Priority level : {Priority::Low, Priority::Medium}) {
        // Подія не повинна витісняти рівних собі або важливіших: інакше
        // потік однотипних подій витіснив би сам себе, а користувач
        // отримав би випадковий зріз замість послідовної картини.
        if (static_cast<uint8_t>(level) >= static_cast<uint8_t>(incomingPriority)) break;

        for (auto it = events_.begin(); it != events_.end();) {
            const bool enoughRoom =
                events_.size() < maxEvents_ && currentBytes_ + incomingBytes <= maxBytes_;
            if (enoughRoom) return true;

            if (it->priority != level) { ++it; continue; }

            currentBytes_ -= it->approximateBytes();
            if (level == Priority::Low) stats_.droppedLow += 1;
            else stats_.droppedMedium += 1;

            it = events_.erase(it);
        }
    }

    return events_.size() < maxEvents_ && currentBytes_ + incomingBytes <= maxBytes_;
}

bool EventQueue::Push(Event&& event) {
    const size_t incomingBytes = event.approximateBytes();

    // Одна подія не може займати помітну частку всієї черги.
    // Такий випадок означає помилку нормалізації вище за течією.
    if (incomingBytes > maxBytes_ / 4) {
        Guard guard(lock_);
        stats_.droppedOversize += 1;
        return false;
    }

    Guard guard(lock_);

    const bool needsRoom =
        events_.size() >= maxEvents_ || currentBytes_ + incomingBytes > maxBytes_;

    if (needsRoom && !MakeRoom(incomingBytes, event.priority)) {
        // Місця немає навіть після витіснення. Подія високого пріоритету
        // все одно має потрапити в чергу — жертвуємо найстарішою.
        if (event.priority == Priority::High && !events_.empty()) {
            currentBytes_ -= events_.front().approximateBytes();
            stats_.droppedMedium += 1;
            events_.pop_front();
        } else {
            if (event.priority == Priority::Low) stats_.droppedLow += 1;
            else stats_.droppedMedium += 1;
            return false;
        }
    }

    currentBytes_ += incomingBytes;
    events_.push_back(std::move(event));
    stats_.pushed += 1;
    stats_.peakCount = std::max(stats_.peakCount, events_.size());
    return true;
}

size_t EventQueue::Drain(std::vector<Event>& out, size_t maxCount) {
    Guard guard(lock_);

    const size_t count = std::min(maxCount, events_.size());
    out.reserve(out.size() + count);

    for (size_t i = 0; i < count; ++i) {
        currentBytes_ -= events_.front().approximateBytes();
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    stats_.popped += count;
    return count;
}

void EventQueue::Requeue(std::vector<Event>&& events) {
    if (events.empty()) return;

    Guard guard(lock_);

    // Повертаємо на початок черги у вихідному порядку, щоб не порушити
    // послідовність нумерації для приймача.
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (events_.size() >= maxEvents_) {
            // Місця вже немає: втрачаємо найменш важливе з поверненого,
            // а не свіжіші події, які вже встигли накопичитись.
            if (it->priority == Priority::Low) { stats_.droppedLow += 1; continue; }
            if (events_.empty()) break;
            currentBytes_ -= events_.back().approximateBytes();
            events_.pop_back();
            stats_.droppedMedium += 1;
        }
        currentBytes_ += it->approximateBytes();
        events_.push_front(std::move(*it));
    }
    events.clear();
}

void EventQueue::Clear() {
    Guard guard(lock_);
    events_.clear();
    currentBytes_ = 0;
}

bool EventQueue::empty() const {
    Guard guard(lock_);
    return events_.empty();
}

size_t EventQueue::size() const {
    Guard guard(lock_);
    return events_.size();
}

QueueStats EventQueue::stats() const {
    Guard guard(lock_);
    QueueStats snapshot = stats_;
    snapshot.currentCount = events_.size();
    snapshot.currentBytes = currentBytes_;
    return snapshot;
}

}  // namespace cm
