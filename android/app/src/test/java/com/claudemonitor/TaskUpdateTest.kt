package com.claudemonitor

import com.claudemonitor.data.Protocol
import com.claudemonitor.data.updatedBy
import com.claudemonitor.ui.formatDuration
import com.claudemonitor.i18n.UkStrings
import com.claudemonitor.ui.formatTimer
import org.junit.Assert.assertEquals
import org.junit.Test

/** Тести оновлення картки задачі, насамперед таймера відповіді. */
class TaskUpdateTest {

    private fun task(
        state: Protocol.ClaudeState = Protocol.ClaudeState.IDLE,
        turnStartedAtMs: Long = 0,
        lastTurnMs: Long = 0,
    ) = Protocol.TaskSession(
        sessionId = "s1",
        short = "s1",
        title = "задача",
        titleSource = Protocol.TitleSource.PROMPT,
        project = "demo",
        cwd = null,
        branch = null,
        state = state,
        activity = null,
        startedAtMs = 1,
        lastUpdateMs = 1,
        sequence = 1,
        turnStartedAtMs = turnStartedAtMs,
        lastTurnMs = lastTurnMs,
    )

    private fun status(
        state: Protocol.ClaudeState,
        turnStartedAtMs: Long = 0,
        turnMs: Long = 0,
    ) = Protocol.MonitorEvent(
        sessionId = "s1",
        sequence = 2,
        timestampMs = 2,
        kind = Protocol.EventKind.STATUS,
        state = state,
        turnStartedAtMs = turnStartedAtMs,
        turnMs = turnMs,
    )

    @Test
    fun `початок відповіді запускає таймер`() {
        val updated = task().updatedBy(status(Protocol.ClaudeState.WORKING, turnStartedAtMs = 1000))
        assertEquals(1000L, updated.turnStartedAtMs)
    }

    @Test
    fun `кінець відповіді зупиняє таймер і зберігає тривалість`() {
        val running = task(Protocol.ClaudeState.WORKING, turnStartedAtMs = 1000)
        val updated = running.updatedBy(status(Protocol.ClaudeState.IDLE, turnMs = 192_500))

        assertEquals(0L, updated.turnStartedAtMs)
        assertEquals(192_500L, updated.lastTurnMs)
    }

    @Test
    fun `помилка посеред відповіді таймер не зупиняє`() {
        val running = task(Protocol.ClaudeState.WORKING, turnStartedAtMs = 1000)
        val updated = running.updatedBy(status(Protocol.ClaudeState.ERROR))
        assertEquals(1000L, updated.turnStartedAtMs)
    }

    @Test
    fun `тиша без тривалості не стирає попередню тривалість`() {
        // Bridge визнав задачу неактивною через тишу — скільки тривала
        // відповідь, він ще не знає, тож старе значення лишається.
        val running = task(Protocol.ClaudeState.WORKING, turnStartedAtMs = 1000, lastTurnMs = 30_000)
        val updated = running.updatedBy(status(Protocol.ClaudeState.IDLE))

        assertEquals(0L, updated.turnStartedAtMs)
        assertEquals(30_000L, updated.lastTurnMs)
    }

    @Test
    fun `подія дії таймера не торкається`() {
        val running = task(Protocol.ClaudeState.WORKING, turnStartedAtMs = 1000, lastTurnMs = 5000)
        val activity = Protocol.MonitorEvent(
            sessionId = "s1",
            sequence = 3,
            timestampMs = 3,
            kind = Protocol.EventKind.ACTIVITY,
            action = "run_command",
            target = "ls",
        )
        val updated = running.updatedBy(activity)

        assertEquals(1000L, updated.turnStartedAtMs)
        assertEquals(5000L, updated.lastTurnMs)
        assertEquals("ls", updated.activity?.target)
    }

    @Test
    fun `тривалість словами`() {
        assertEquals("12 с", formatDuration(12_000, UkStrings))
        assertEquals("3 хв 12 с", formatDuration(192_400, UkStrings))
        assertEquals("3 хв", formatDuration(180_000, UkStrings))
        assertEquals("1 год 5 хв", formatDuration(3_900_000, UkStrings))
    }

    @Test
    fun `таймер циферблатом і без від'ємного часу`() {
        assertEquals("0:07", formatTimer(7_900))
        assertEquals("3:12", formatTimer(192_000))
        assertEquals("1:05:07", formatTimer(3_907_000))
        // Годинник телефона трохи відстає від ноутбука.
        assertEquals("0:00", formatTimer(-2_000))
    }
}
