package com.claudemonitor

import com.claudemonitor.data.Protocol.ClaudeState
import com.claudemonitor.data.TaskFilter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Тести розкладання задач по вкладках головного екрана. */
class TaskFilterTest {

    @Test
    fun `задача що працює належить до активних`() {
        assertTrue(TaskFilter.ACTIVE.matches(ClaudeState.WORKING))
        assertFalse(TaskFilter.INACTIVE.matches(ClaudeState.WORKING))
    }

    @Test
    fun `задача що чекає на ваш дозвіл належить до активних`() {
        assertTrue(TaskFilter.ACTIVE.matches(ClaudeState.WAITING))
    }

    @Test
    fun `задача що очікує наступного запиту неактивна`() {
        assertTrue(TaskFilter.INACTIVE.matches(ClaudeState.IDLE))
        assertFalse(TaskFilter.ACTIVE.matches(ClaudeState.IDLE))
    }

    @Test
    fun `завершена й зникла задачі неактивні`() {
        assertTrue(TaskFilter.INACTIVE.matches(ClaudeState.FINISHED))
        assertTrue(TaskFilter.INACTIVE.matches(ClaudeState.GONE))
    }

    @Test
    fun `невідомий стан не вважається активним`() {
        // Назвати задачу працюючою без доказу — означало б вигадати стан.
        assertFalse(TaskFilter.ACTIVE.matches(ClaudeState.UNKNOWN))
    }

    @Test
    fun `вкладка всі показує кожен стан`() {
        for (state in ClaudeState.entries) {
            assertTrue(state.name, TaskFilter.ALL.matches(state))
        }
    }

    @Test
    fun `кожна задача рівно в одній із вкладок активні чи неактивні`() {
        for (state in ClaudeState.entries) {
            val inActive = TaskFilter.ACTIVE.matches(state)
            val inInactive = TaskFilter.INACTIVE.matches(state)
            assertEquals(state.name, 1, listOf(inActive, inInactive).count { it })
        }
    }
}
