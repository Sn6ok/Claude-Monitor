// test_queue.cpp — тести обмеженої черги подій.
//
// Головна властивість, яку тут перевіряють: черга НІКОЛИ не росте
// необмежено, а при переповненні втрачає найменш важливе, зберігаючи
// зміни стану й помилки (docs/protocol.md §8).

#include "test_framework.h"
#include "queue.h"

using namespace cm;

namespace {

Event MakeEvent(Priority priority, std::string_view sessionId = "s1", size_t textSize = 16) {
    Event event;
    event.sessionId = sessionId;
    event.priority = priority;
    event.kind = priority == Priority::High ? EventKind::Status : EventKind::Output;
    event.text.assign(textSize, 'x');
    event.timestampMs = NowUnixMs();
    return event;
}

}  // namespace

CM_TEST(queue, push_and_drain_preserve_order) {
    EventQueue queue(100, 1024 * 1024);

    for (int i = 0; i < 5; ++i) {
        Event event = MakeEvent(Priority::Medium);
        event.sequence = static_cast<uint64_t>(i);
        CHECK(queue.Push(std::move(event)));
    }

    std::vector<Event> drained;
    CHECK_EQ(queue.Drain(drained, 10), 5u);
    CHECK_EQ(drained.size(), 5u);

    for (size_t i = 0; i < drained.size(); ++i) {
        CHECK_EQ(drained[i].sequence, static_cast<uint64_t>(i));
    }
    CHECK(queue.empty());
}

CM_TEST(queue, respects_event_count_limit) {
    EventQueue queue(10, 1024 * 1024);

    for (int i = 0; i < 100; ++i) {
        queue.Push(MakeEvent(Priority::Low));
    }

    // Головне: розмір не перевищив межу, попри стократне перевищення спроб.
    CHECK(queue.size() <= 10u);
}

CM_TEST(queue, respects_byte_limit) {
    // Мала межа в байтах при великій межі за кількістю.
    EventQueue queue(1000, 4096);

    for (int i = 0; i < 200; ++i) {
        queue.Push(MakeEvent(Priority::Low, "s1", 100));
    }

    const QueueStats stats = queue.stats();
    CHECK(stats.currentBytes <= 4096u);
}

CM_TEST(queue, high_priority_survives_overflow) {
    EventQueue queue(10, 1024 * 1024);

    // Заповнюємо чергу подіями низького пріоритету.
    for (int i = 0; i < 10; ++i) queue.Push(MakeEvent(Priority::Low));

    Event important = MakeEvent(Priority::High);
    important.sequence = 9999;
    CHECK(queue.Push(std::move(important)));

    // Подія високого пріоритету має потрапити в чергу навіть тоді,
    // коли місця немає: саме на цьому тримається гарантія, що
    // користувач побачить зміну стану чи помилку.
    std::vector<Event> drained;
    queue.Drain(drained, 100);

    bool found = false;
    for (const Event& event : drained) {
        if (event.sequence == 9999) { found = true; break; }
    }
    CHECK(found);
}

CM_TEST(queue, low_priority_evicted_before_medium) {
    EventQueue queue(6, 1024 * 1024);

    for (int i = 0; i < 3; ++i) queue.Push(MakeEvent(Priority::Low));
    for (int i = 0; i < 3; ++i) queue.Push(MakeEvent(Priority::Medium));

    // Тепер додаємо ще події середнього пріоритету: витіснятися мають
    // саме низькопріоритетні.
    for (int i = 0; i < 3; ++i) queue.Push(MakeEvent(Priority::Medium));

    const QueueStats stats = queue.stats();
    CHECK(stats.droppedLow > 0);

    std::vector<Event> drained;
    queue.Drain(drained, 100);

    size_t lowCount = 0;
    for (const Event& event : drained) {
        if (event.priority == Priority::Low) ++lowCount;
    }
    CHECK(lowCount < 3u);
}

CM_TEST(queue, oversized_event_is_rejected) {
    EventQueue queue(100, 4096);

    // Одна подія не може займати помітну частку черги.
    Event huge = MakeEvent(Priority::Medium, "s1", 4000);
    CHECK_FALSE(queue.Push(std::move(huge)));

    const QueueStats stats = queue.stats();
    CHECK_EQ(stats.droppedOversize, 1u);
}

CM_TEST(queue, requeue_restores_order) {
    EventQueue queue(100, 1024 * 1024);

    for (int i = 0; i < 3; ++i) {
        Event event = MakeEvent(Priority::Medium);
        event.sequence = static_cast<uint64_t>(i);
        queue.Push(std::move(event));
    }

    std::vector<Event> batch;
    queue.Drain(batch, 3);

    // Додаємо нову подію, потім повертаємо невдалу партію.
    Event newer = MakeEvent(Priority::Medium);
    newer.sequence = 100;
    queue.Push(std::move(newer));

    queue.Requeue(std::move(batch));

    std::vector<Event> drained;
    queue.Drain(drained, 10);

    // Повернені події мають опинитися попереду, у вихідному порядку:
    // інакше приймач побачив би розрив у нумерації.
    CHECK_EQ(drained.size(), 4u);
    CHECK_EQ(drained[0].sequence, 0u);
    CHECK_EQ(drained[1].sequence, 1u);
    CHECK_EQ(drained[2].sequence, 2u);
    CHECK_EQ(drained[3].sequence, 100u);
}

CM_TEST(queue, drain_partial_batch) {
    EventQueue queue(100, 1024 * 1024);
    for (int i = 0; i < 10; ++i) queue.Push(MakeEvent(Priority::Medium));

    std::vector<Event> batch;
    CHECK_EQ(queue.Drain(batch, 4), 4u);
    CHECK_EQ(queue.size(), 6u);
}

CM_TEST(queue, memory_does_not_grow_without_bound) {
    EventQueue queue(kMaxQueueEvents, 2 * 1024 * 1024);

    // Імітуємо тривалу роботу без мережі: 50 000 подій за жодних умов
    // не мають вичерпати пам'ять (Частина 5 §16 Master Prompt).
    for (int i = 0; i < 50'000; ++i) {
        queue.Push(MakeEvent(i % 10 == 0 ? Priority::High : Priority::Low, "s1", 64));
    }

    const QueueStats stats = queue.stats();
    CHECK(stats.currentCount <= kMaxQueueEvents);
    CHECK(stats.currentBytes <= 2 * 1024 * 1024);
    CHECK(stats.droppedLow > 0);
}

CM_TEST(queue, events_from_multiple_sessions_coexist) {
    EventQueue queue(100, 1024 * 1024);

    queue.Push(MakeEvent(Priority::Medium, "session-A"));
    queue.Push(MakeEvent(Priority::Medium, "session-B"));
    queue.Push(MakeEvent(Priority::Medium, "session-C"));

    std::vector<Event> drained;
    queue.Drain(drained, 10);

    CHECK_EQ(drained.size(), 3u);
    // Кожна подія зберігає свою задачу: черга нічого не перемішує.
    CHECK_STR(drained[0].sessionId, "session-A");
    CHECK_STR(drained[1].sessionId, "session-B");
    CHECK_STR(drained[2].sessionId, "session-C");
}

CM_TEST(queue, clear_resets_state) {
    EventQueue queue(100, 1024 * 1024);
    for (int i = 0; i < 10; ++i) queue.Push(MakeEvent(Priority::Medium));

    queue.Clear();
    CHECK(queue.empty());
    CHECK_EQ(queue.stats().currentBytes, 0u);
}
