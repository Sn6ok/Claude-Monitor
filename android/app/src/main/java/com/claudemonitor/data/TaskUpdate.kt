package com.claudemonitor.data

/**
 * Застосовує подію до картки задачі.
 *
 * Правила винесено окремо від ViewModel, щоб їх — насамперед таймер
 * відповіді — можна було перевіряти звичайними тестами.
 *
 * Таймер: подія про стан посеред відповіді несе її початок, і картка
 * його запам'ятовує. Коли задача стає неактивною, живий відлік зупиняється,
 * а тривалість завершеної відповіді, якщо Bridge її знає, зберігається.
 */
fun Protocol.TaskSession.updatedBy(event: Protocol.MonitorEvent): Protocol.TaskSession {
    val isStatus = event.kind == Protocol.EventKind.STATUS
    val newState = event.state ?: state

    val turnStart = when {
        !isStatus -> turnStartedAtMs
        event.turnStartedAtMs > 0 -> event.turnStartedAtMs
        // Відповідь завершилась або задача закрилась — відлік зупиняється.
        !TaskFilter.isActive(newState) -> 0L
        // Помилка інструмента чи очікування дозволу — відповідь триває.
        else -> turnStartedAtMs
    }

    return copy(
        state = newState,
        // Довга команда приходить частинами; картці досить початку.
        activity = if (event.kind == Protocol.EventKind.ACTIVITY && event.part == 0) {
            Protocol.Activity(event.action ?: "other", event.target)
        } else {
            activity
        },
        turnStartedAtMs = turnStart,
        lastTurnMs = if (isStatus && event.turnMs > 0) event.turnMs else lastTurnMs,
        lastUpdateMs = maxOf(lastUpdateMs, event.timestampMs),
        sequence = maxOf(sequence, event.sequence),
    )
}
