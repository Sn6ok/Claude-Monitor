package com.claudemonitor

import com.claudemonitor.data.Protocol.ClaudeState
import com.claudemonitor.ui.MarkerKind
import com.claudemonitor.ui.formatLimitReset
import com.claudemonitor.ui.statusMarker
import com.claudemonitor.i18n.UkStrings
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** Тести позначок у стрічці: «готово», «перервано», «ліміт». */
class StatusMarkerTest {

    @Test
    fun `кінець відповіді позначається як готово з тривалістю`() {
        val marker = statusMarker(ClaudeState.IDLE, null, 85_400, UkStrings)
        assertEquals("ГОТОВО", marker.label)
        assertEquals("1 хв 25 с", marker.detail)
        assertEquals(MarkerKind.DONE, marker.kind)
    }

    @Test
    fun `перервана відповідь має власну позначку`() {
        val marker = statusMarker(ClaudeState.IDLE, "перервано", 86_000, UkStrings)
        assertEquals("ПЕРЕРВАНО", marker.label)
        assertEquals("1 хв 26 с", marker.detail)
    }

    @Test
    fun `ліміт показує час скидання й тривалість відповіді`() {
        val marker = statusMarker(ClaudeState.LIMITED, "10:30pm (Europe/Kyiv)", 85_000, UkStrings)
        assertEquals("ЛІМІТ", marker.label)
        assertEquals("до 22:30  ·  1 хв 25 с", marker.detail)
        assertEquals(MarkerKind.LIMIT, marker.kind)
    }

    @Test
    fun `очікування без тривалості лишається очікуванням`() {
        val marker = statusMarker(ClaudeState.IDLE, null, 0, UkStrings)
        assertEquals("ОЧІКУЄ", marker.label)
        assertNull(marker.detail)
    }

    @Test
    fun `тривалість у позначці збігається з таймером`() {
        // Таймер угорі відкидає частки секунди — позначка теж.
        assertEquals("1 хв 24 с", statusMarker(ClaudeState.IDLE, null, 84_999, UkStrings).detail)
    }

    @Test
    fun `час скидання ліміту у звичному вигляді`() {
        assertEquals("22:30", formatLimitReset("10:30pm (Europe/Kyiv)"))
        assertEquals("17:00", formatLimitReset("5pm (Europe/Kyiv)"))
        assertEquals("04:30", formatLimitReset("4:30am (Europe/Kyiv)"))
        assertEquals("00:00", formatLimitReset("12am (Europe/Kyiv)"))
        assertEquals("23.08, 17:00", formatLimitReset("Aug 23, 5pm (Europe/Kyiv)"))
        assertEquals("завтра", formatLimitReset("завтра (Europe/Kyiv)"))
    }
}
