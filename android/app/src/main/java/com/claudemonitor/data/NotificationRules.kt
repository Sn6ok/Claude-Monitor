package com.claudemonitor.data

/** Про що саме сповіщати. */
enum class TaskNotification { STARTED, FINISHED, INTERRUPTED, LIMIT, WAITING }

/**
 * Коли зміна стану задачі варта сповіщення.
 *
 * Правила винесено окремо, щоб їх можна було перевіряти звичайними тестами.
 * Сповіщення будується лише на справжній зміні стану: той самий стан двічі
 * поспіль нічого не означає, а вигадувати подію, якої не було, не можна.
 */
object NotificationRules {

    /** Текст, яким Bridge позначає перервану відповідь. */
    const val INTERRUPTED_NOTE = "перервано"

    /**
     * @param previous стан до зміни; null — задача ще не була відома
     * @param next стан після зміни
     * @param note подробиця зміни від Bridge (наприклад, «перервано»)
     */
    fun forTransition(
        previous: Protocol.ClaudeState?,
        next: Protocol.ClaudeState,
        note: String?,
    ): TaskNotification? {
        // Щойно побачена задача — це не зміна, а перше знайомство з нею.
        if (previous == null || previous == next) return null

        return when (next) {
            // Після запиту дозволу чи помилки інструмента відповідь просто
            // триває — нового початку роботи тут немає.
            Protocol.ClaudeState.WORKING -> when (previous) {
                Protocol.ClaudeState.WAITING, Protocol.ClaudeState.ERROR -> null
                else -> TaskNotification.STARTED
            }

            Protocol.ClaudeState.IDLE -> when {
                // Ліміт скинувся сам — Claude нічого не завершував.
                previous == Protocol.ClaudeState.LIMITED -> null
                note == INTERRUPTED_NOTE -> TaskNotification.INTERRUPTED
                previous == Protocol.ClaudeState.WORKING ||
                    previous == Protocol.ClaudeState.WAITING ||
                    previous == Protocol.ClaudeState.ERROR -> TaskNotification.FINISHED
                else -> null
            }

            Protocol.ClaudeState.LIMITED -> TaskNotification.LIMIT
            Protocol.ClaudeState.WAITING -> TaskNotification.WAITING
            else -> null
        }
    }
}

/** Чи дозволив користувач саме такі сповіщення. */
fun AppSettings.allows(kind: TaskNotification): Boolean {
    if (!notificationsEnabled) return false
    return when (kind) {
        TaskNotification.STARTED -> notifyStarted
        TaskNotification.FINISHED, TaskNotification.INTERRUPTED -> notifyFinished
        TaskNotification.LIMIT -> notifyLimit
        TaskNotification.WAITING -> notifyWaiting
    }
}
