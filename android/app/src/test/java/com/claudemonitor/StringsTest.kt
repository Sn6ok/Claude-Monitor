package com.claudemonitor

import com.claudemonitor.data.Protocol
import com.claudemonitor.data.Protocol.ClaudeState
import com.claudemonitor.i18n.EnStrings
import com.claudemonitor.i18n.UkStrings
import com.claudemonitor.ui.formatAge
import com.claudemonitor.ui.formatDuration
import com.claudemonitor.ui.stateLabel
import com.claudemonitor.ui.statusMarker
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

class StringsTest {

    @Test
    fun englishDurations() {
        assertEquals("12 s", formatDuration(12_000, EnStrings))
        assertEquals("3 min 12 s", formatDuration(192_400, EnStrings))
        assertEquals("1 h 5 min", formatDuration(3_900_000, EnStrings))
    }

    @Test
    fun englishMarkers() {
        val limit = statusMarker(ClaudeState.LIMITED, "10:30pm (Europe/Kyiv)", 85_000, EnStrings)
        assertEquals("LIMIT", limit.label)
        assertEquals("until 22:30  ·  1 min 25 s", limit.detail)

        assertEquals("DONE", statusMarker(ClaudeState.IDLE, null, 5_000, EnStrings).label)
        assertEquals("INTERRUPTED", statusMarker(ClaudeState.IDLE, "перервано", 5_000, EnStrings).label)
    }

    @Test
    fun bridgeNotesAreTranslated() {
        val marker = statusMarker(ClaudeState.ERROR, "помилка без опису", 0, EnStrings)
        assertEquals("error without description", marker.detail)
        // Українською подробиця лишається як є.
        assertEquals("помилка без опису", statusMarker(ClaudeState.ERROR, "помилка без опису", 0, UkStrings).detail)
    }

    @Test
    fun attachedImagesUseCorrectPluralForms() {
        assertEquals("\uD83D\uDCCE 1 зображення", UkStrings.imagesAttached(1))
        assertEquals("\uD83D\uDCCE 3 зображення", UkStrings.imagesAttached(3))
        assertEquals("\uD83D\uDCCE 5 зображень", UkStrings.imagesAttached(5))
        assertEquals("\uD83D\uDCCE 11 зображень", UkStrings.imagesAttached(11))
        assertEquals("\uD83D\uDCCE 21 зображення", UkStrings.imagesAttached(21))
        assertEquals("\uD83D\uDCCE 1 image", EnStrings.imagesAttached(1))
        assertEquals("\uD83D\uDCCE 2 images", EnStrings.imagesAttached(2))
    }

    @Test
    fun languagesReallyDiffer() {
        assertEquals("ОЧІКУЄ", stateLabel(ClaudeState.IDLE, UkStrings))
        assertEquals("IDLE", stateLabel(ClaudeState.IDLE, EnStrings))
        assertNotEquals(formatAge(1_000, 31_000, UkStrings), formatAge(1_000, 31_000, EnStrings))
    }

    @Test
    fun relayAddressErrorsFollowLanguage() {
        assertEquals(EnStrings.urlEmpty, Protocol.validateRelayUrl("", EnStrings))
        assertEquals(UkStrings.urlScheme, Protocol.validateRelayUrl("https://example.com", UkStrings))
        assertEquals(EnStrings.urlPlainPublic, Protocol.validateRelayUrl("ws://example.com/ws", EnStrings))
        assertNull(Protocol.validateRelayUrl("wss://example.com/ws", EnStrings))
        assertNull(Protocol.validateRelayUrl("ws://192.168.1.10:8080/ws", EnStrings))
    }
}
