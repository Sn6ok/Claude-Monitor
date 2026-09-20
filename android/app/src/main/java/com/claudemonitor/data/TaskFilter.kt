package com.claudemonitor.data

/**
 * Вкладки головного екрана.
 *
 * «Активна» задача — та, що просто зараз щось робить або потребує уваги:
 * працює, запускається, чекає на ваш дозвіл або щойно отримала помилку
 * посеред роботи. Решта — неактивні: хід завершено й задача чекає
 * наступного запиту, задачу закрито або її стан невідомий.
 *
 * Невідомий стан свідомо не вважається активним: назвати задачу працюючою
 * без доказу означало б вигадати її стан (Частина 5 §12 Master Prompt).
 */
enum class TaskFilter {
    ACTIVE,
    INACTIVE,
    ALL;

    fun matches(state: Protocol.ClaudeState): Boolean = when (this) {
        ALL -> true
        ACTIVE -> isActive(state)
        INACTIVE -> !isActive(state)
    }

    companion object {
        fun isActive(state: Protocol.ClaudeState): Boolean = when (state) {
            Protocol.ClaudeState.WORKING,
            Protocol.ClaudeState.WAITING,
            Protocol.ClaudeState.STARTING,
            Protocol.ClaudeState.ERROR -> true
            else -> false
        }
    }
}
