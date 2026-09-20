package com.claudemonitor

import com.claudemonitor.data.EventHistory
import com.claudemonitor.data.Protocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Тести накопичення історії.
 *
 * Головне, що тут перевіряється, — отримане не зникає: ні від чергового
 * знімка стану, ні від перезапуску Bridge. Саме це ламалося раніше.
 */
class EventHistoryTest {

    private fun output(seq: Long, ts: Long, text: String, sid: String = "s1") =
        Protocol.MonitorEvent(
            sessionId = sid,
            sequence = seq,
            timestampMs = ts,
            kind = Protocol.EventKind.OUTPUT,
            text = text,
        )

    private fun part(seq: Long, group: Long, index: Int, count: Int, text: String, sid: String = "s1") =
        Protocol.MonitorEvent(
            sessionId = sid,
            sequence = seq,
            timestampMs = 1000,
            kind = Protocol.EventKind.OUTPUT,
            text = text,
            groupId = group,
            part = index,
            partCount = count,
        )

    private fun historyOf(range: LongRange, seqOffset: Long = 0): List<Protocol.MonitorEvent> {
        var history = emptyList<Protocol.MonitorEvent>()
        for (i in range) history = EventHistory.append(history, output(seqOffset + i, 1000 + i, "відповідь $i"))
        return history
    }

    @Test
    fun `знімок не стирає накопичену історію`() {
        val history = historyOf(1..50L)

        // Знімок несе лише кілька останніх подій — раніше саме він затирав усе.
        val snapshot = (46..50L).map { output(it, 1000 + it, "відповідь $it") }
        val merged = EventHistory.mergeSnapshot(history, snapshot)

        assertEquals(50, merged.size)
        assertEquals("відповідь 1", merged.first().text)
    }

    @Test
    fun `знімок додає пропущене поки не було зв'язку`() {
        val history = historyOf(1..3L)
        val snapshot = (2..5L).map { output(it, 1000 + it, "відповідь $it") }

        val merged = EventHistory.mergeSnapshot(history, snapshot)

        assertEquals((1..5).map { "відповідь $it" }, merged.map { it.text })
    }

    @Test
    fun `перезапуск Bridge не задвоює історію`() {
        // Після перезапуску номери послідовності починаються заново,
        // але самі події ті самі.
        val history = historyOf(1..3L, seqOffset = 100)
        val afterRestart = (1..3L).map { output(it, 1000 + it, "відповідь $it") }

        assertEquals(3, EventHistory.mergeSnapshot(history, afterRestart).size)
    }

    @Test
    fun `частини довгої репліки склеюються в одне повідомлення`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, part(1, 7, 0, 3, "перша "))
        history = EventHistory.append(history, part(2, 7, 1, 3, "друга "))
        history = EventHistory.append(history, part(3, 7, 2, 3, "третя"))

        assertEquals(1, history.size)
        assertEquals("перша друга третя", history[0].text)
    }

    @Test
    fun `повторна частина не дублює текст`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, part(1, 7, 0, 2, "перша "))
        history = EventHistory.append(history, part(2, 7, 1, 2, "друга"))
        history = EventHistory.append(history, part(2, 7, 1, 2, "друга"))

        assertEquals("перша друга", history[0].text)
    }

    @Test
    fun `пропущена частина позначається а не зникає мовчки`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, part(1, 7, 0, 3, "перша"))
        history = EventHistory.append(history, part(3, 7, 2, 3, "третя"))

        assertEquals(1, history.size)
        val text = history[0].text!!
        assertTrue(text.contains("…"))
        assertTrue(text.endsWith("третя"))
    }

    @Test
    fun `частини різних задач не склеюються`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, part(1, 7, 0, 2, "задача А"))
        history = EventHistory.append(history, part(2, 7, 1, 2, "задача Б", sid = "s2"))

        assertEquals(2, history.size)
    }

    @Test
    fun `довга репліка зі знімка не задвоюється частинами`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, part(1, 7, 0, 2, "перша "))
        history = EventHistory.append(history, part(2, 7, 1, 2, "друга"))

        // У знімку ті самі частини, скорочені, а після перезапуску — ще й
        // з іншим номером групи.
        val snapshot = listOf(part(10, 99, 0, 2, "пер"), part(11, 99, 1, 2, "дру"))
        val merged = EventHistory.mergeSnapshot(history, snapshot)

        assertEquals(1, merged.size)
        assertEquals("перша друга", merged[0].text)
    }

    @Test
    fun `частини довгої команди склеюються в повну ціль`() {
        fun commandPart(seq: Long, index: Int, piece: String) = Protocol.MonitorEvent(
            sessionId = "s1",
            sequence = seq,
            timestampMs = 7000,
            kind = Protocol.EventKind.ACTIVITY,
            action = "run_command",
            target = piece,
            groupId = 9,
            part = index,
            partCount = 2,
        )

        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, commandPart(1, 0, "python - <<'PY'\nprint(1)"))
        history = EventHistory.append(history, commandPart(2, 1, "\nPY"))

        assertEquals(1, history.size)
        assertEquals("python - <<'PY'\nprint(1)\nPY", history[0].target)
    }

    @Test
    fun `записи різних файлів з однієї теки не зливаються`() {
        // Шляхи збігаються на перших десятках символів, а час — до мілісекунди:
        // Claude записує кілька файлів одним повідомленням.
        val folder = "D:\\Wall\\For Hacker\\Claude Monito\\claude-monitor\\android\\app\\src\\main\\java\\ui\\"
        val snapshot = listOf("Markdown.kt", "SyntaxHighlight.kt", "MarkdownPalette.kt").mapIndexed { index, file ->
            Protocol.MonitorEvent(
                sessionId = "s1",
                sequence = index.toLong() + 1,
                timestampMs = 5000,
                kind = Protocol.EventKind.ACTIVITY,
                action = "write_file",
                target = folder + file,
            )
        }

        assertEquals(3, EventHistory.mergeSnapshot(emptyList(), snapshot).size)
    }

    @Test
    fun `ключі записів унікальні навіть за однакових номерів`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, output(1, 1000, "до перезапуску"))
        history = EventHistory.append(history, output(1, 2000, "після перезапуску"))

        assertTrue(history.all { it.localId != 0L })
        assertEquals(2, history.map { it.localId }.toSet().size)
    }

    @Test
    fun `історія обмежена за кількістю подій`() {
        var history = emptyList<Protocol.MonitorEvent>()
        for (i in 1..(EventHistory.MAX_EVENTS + 10).toLong()) {
            history = EventHistory.append(history, output(i, i, "x"))
        }

        assertEquals(EventHistory.MAX_EVENTS, history.size)
        // Витісняється найдавніше, найновіше лишається.
        assertEquals(11L, history.first().sequence)
        assertEquals((EventHistory.MAX_EVENTS + 10).toLong(), history.last().sequence)
    }

    @Test
    fun `історія обмежена за обсягом тексту`() {
        val big = "я".repeat(100_000)
        var history = emptyList<Protocol.MonitorEvent>()
        for (i in 1..30L) history = EventHistory.append(history, output(i, i, big))

        assertTrue(history.sumOf { it.text!!.length } <= EventHistory.MAX_TEXT_CHARS)
        assertEquals(30L, history.last().sequence)
    }

    private fun event(
        kind: Protocol.EventKind,
        ts: Long,
        text: String,
        state: Protocol.ClaudeState? = null,
    ) = Protocol.MonitorEvent(
        sessionId = "s1",
        sequence = ts,
        timestampMs = ts,
        kind = kind,
        state = state,
        text = text,
    )

    @Test
    fun `повідомлення посеред відповіді стає після неї`() {
        // Так було в сесії 2dfd7643: друге повідомлення записалося у файл одразу,
        // а відповідь, яку Claude почав писати на три секунди раніше, — лише
        // коли Claude її дописав.
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, event(Protocol.EventKind.PROMPT, 1_000, "знову така ж проблема"))
        history = EventHistory.append(
            history, event(Protocol.EventKind.STATUS, 1_000, "працює", Protocol.ClaudeState.WORKING),
        )
        history = EventHistory.append(history, event(Protocol.EventKind.PROMPT, 3_700, "так ще дивись"))
        history = EventHistory.append(history, event(Protocol.EventKind.OUTPUT, 3_400, "Ти правий"))

        assertEquals(
            listOf("знову така ж проблема", "працює", "Ти правий", "так ще дивись"),
            history.map { it.text },
        )
    }

    @Test
    fun `однаковий час зберігає порядок надходження`() {
        // Позначка кінця відповіді має час останнього блоку й стоїть після нього.
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, event(Protocol.EventKind.OUTPUT, 5_000, "відповідь"))
        history = EventHistory.append(
            history, event(Protocol.EventKind.STATUS, 5_000, "готово", Protocol.ClaudeState.IDLE),
        )

        assertEquals(listOf("відповідь", "готово"), history.map { it.text })
    }

    @Test
    fun `запис без часу додається в кінець`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, event(Protocol.EventKind.OUTPUT, 5_000, "перша"))
        history = EventHistory.append(
            history, event(Protocol.EventKind.STATUS, 0, "без часу", Protocol.ClaudeState.IDLE),
        )

        assertEquals("без часу", history.last().text)
    }

    @Test
    fun `пропущене зі знімка стає на своє місце в часі`() {
        var history = emptyList<Protocol.MonitorEvent>()
        history = EventHistory.append(history, output(1, 1000, "перша"))
        history = EventHistory.append(history, output(3, 3000, "третя"))

        val merged = EventHistory.mergeSnapshot(history, listOf(output(2, 2000, "друга")))

        assertEquals(listOf("перша", "друга", "третя"), merged.map { it.text })
    }
}
