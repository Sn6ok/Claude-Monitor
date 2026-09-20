package com.claudemonitor

import com.claudemonitor.data.Crypto
import com.claudemonitor.data.Crypto.fromBase64Url
import com.claudemonitor.data.Crypto.toBase64Url
import com.claudemonitor.data.Protocol
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Тести протоколу та криптографічних допоміжних функцій.
 *
 * Перевіряють дві речі: що застосунок коректно розбирає очікувані дані
 * і що він не ламається на ворожому вході. Друге важливіше — розбір
 * відбувається над даними з мережі.
 */
class ProtocolTest {

    // ── Base64url ────────────────────────────────────────────────────────────

    @Test
    fun `base64url кодує без символів доповнення`() {
        assertEquals("AQ", byteArrayOf(1).toBase64Url())
        assertEquals("AQI", byteArrayOf(1, 2).toBase64Url())
        assertEquals("AQID", byteArrayOf(1, 2, 3).toBase64Url())
        assertEquals("", ByteArray(0).toBase64Url())
    }

    @Test
    fun `base64url використовує безпечний для URL алфавіт`() {
        // Байти, які у звичайному base64 дали б символи + та /.
        val encoded = byteArrayOf(0xFB.toByte(), 0xFF.toByte(), 0xFE.toByte()).toBase64Url()
        assertFalse(encoded.contains('+'))
        assertFalse(encoded.contains('/'))
        assertFalse(encoded.contains('='))
    }

    @Test
    fun `base64url витримує повний цикл`() {
        for (length in 0..64) {
            val original = ByteArray(length) { (it * 7 + 13).toByte() }
            val decoded = original.toBase64Url().fromBase64Url()
            assertNotNull("довжина $length", decoded)
            assertTrue("довжина $length", original.contentEquals(decoded!!))
        }
    }

    @Test
    fun `base64url відхиляє сторонні символи`() {
        assertNull("AQ+D".fromBase64Url())
        assertNull("AQ/D".fromBase64Url())
        assertNull("AQ=D".fromBase64Url())
        assertNull("AQ D".fromBase64Url())
    }

    // ── HKDF ─────────────────────────────────────────────────────────────────

    @Test
    fun `HKDF відповідає контрольному вектору RFC 5869`() {
        // Тест 1 з RFC 5869. Збіг доводить сумісність із Bridge і Relay:
        // усі три реалізації мають давати однаковий результат.
        val ikm = ByteArray(22) { 0x0b }
        val salt = ByteArray(13) { it.toByte() }
        val info = ByteArray(10) { (0xf0 + it).toByte() }

        val okm = Crypto.hkdf(ikm, salt, info, 42)

        val expected = "3cb25f25faacd57a90434f64d0362f2a" +
            "2d2d0a90cf1a5a4c5db02d56ecc4c5bf" +
            "34007208d5b887185865"

        assertEquals(expected, okm.joinToString("") { "%02x".format(it) })
    }

    @Test
    fun `HKDF з порожньою сіллю не падає`() {
        val okm = Crypto.hkdf(ByteArray(32) { 1 }, ByteArray(0), ByteArray(0), 32)
        assertEquals(32, okm.size)
    }

    // ── Код підключення ──────────────────────────────────────────────────────

    @Test
    fun `нормалізація коду прибирає роздільники`() {
        assertEquals("ABCDEFGHJKMN", Protocol.normalizePairingCode("abcd-efgh-jkmn"))
        assertEquals("ABCDEFGHJKMN", Protocol.normalizePairingCode("ABCD EFGH JKMN"))
        assertEquals("ABCDEFGHJKMN", Protocol.normalizePairingCode("  abcd efgh jkmn  "))
    }

    @Test
    fun `нормалізація виправляє схожі символи`() {
        // Користувач переписує код з екрана, тож плутанина неминуча.
        assertEquals("A1CDEFGHJKMN", Protocol.normalizePairingCode("AICD-EFGH-JKMN"))
        assertEquals("A1CDEFGHJKMN", Protocol.normalizePairingCode("ALCD-EFGH-JKMN"))
        assertEquals("A0CDEFGHJKMN", Protocol.normalizePairingCode("AOCD-EFGH-JKMN"))
        assertEquals("AVCDEFGHJKMN", Protocol.normalizePairingCode("AUCD-EFGH-JKMN"))
    }

    @Test
    fun `нормалізація обмежує довжину`() {
        val long = Protocol.normalizePairingCode("ABCDEFGHJKMNPQRSTVWXYZ")
        assertEquals(Protocol.PAIRING_CODE_LENGTH, long.length)
    }

    @Test
    fun `код підтвердження залежить від ролі`() {
        val asMonitor = Protocol.computeConfirm("ABCD1234EFGH", "monitor", "p1", "p2")
        val asBridge = Protocol.computeConfirm("ABCD1234EFGH", "bridge", "p1", "p2")

        assertTrue(asMonitor.isNotEmpty())
        // Різні ролі дають різні підтвердження: інакше підтвердження однієї
        // сторони можна було б повторити як підтвердження іншої.
        assertFalse(asMonitor == asBridge)
    }

    @Test
    fun `код підтвердження залежить від самого коду`() {
        val a = Protocol.computeConfirm("ABCD1234EFGH", "monitor", "p1", "p2")
        val b = Protocol.computeConfirm("ABCD1234EFGX", "monitor", "p1", "p2")
        assertFalse(a == b)
    }

    // ── Розбір кадрів ────────────────────────────────────────────────────────

    @Test
    fun `коректний кадр розбирається`() {
        val raw = """{"v":1,"t":"auth_ok","id":"01HQ8","ts":${System.currentTimeMillis()},
            "p":{"peer_online":true}}"""
        val frame = Protocol.parseFrame(raw)

        assertNotNull(frame)
        assertEquals("auth_ok", frame!!.type)
        assertTrue(frame.payload.optBoolean("peer_online"))
    }

    @Test
    fun `кадр іншої версії відхиляється`() {
        val raw = """{"v":2,"t":"auth_ok","id":"x","ts":${System.currentTimeMillis()},"p":{}}"""
        assertNull(Protocol.parseFrame(raw))
    }

    @Test
    fun `кадр із застарілою позначкою часу відхиляється`() {
        val now = System.currentTimeMillis()
        val stale = now - Protocol.CLOCK_SKEW_MS - 10_000
        val raw = """{"v":1,"t":"auth_ok","id":"x","ts":$stale,"p":{}}"""

        // Це захист від повторного відтворення старих кадрів.
        assertNull(Protocol.parseFrame(raw, now))
    }

    @Test
    fun `завеликий кадр відхиляється без розбору`() {
        val huge = "x".repeat(Protocol.MAX_FRAME_BYTES + 100)
        assertNull(Protocol.parseFrame(huge))
    }

    @Test
    fun `пошкоджені кадри не кидають винятків`() {
        val garbage = listOf(
            "", "{", "не json", "[]", "null", "0",
            """{"v":1}""",
            """{"v":1,"t":""}""",
            """{"v":1,"t":"x","ts":0}""",
            """{"v":1,"t":"x","ts":-5,"p":{}}""",
        )
        for (input in garbage) {
            // Головне — відсутність винятку; результат може бути будь-яким.
            Protocol.parseFrame(input)
        }
    }

    // ── Розбір знімка ────────────────────────────────────────────────────────

    @Test
    fun `знімок із кількома задачами розбирається повністю`() {
        val json = """
        {
          "t":"snapshot","seq":1042,
          "bridge":{"host":"PC","version":"1.0.0","uptime_sec":100,
                    "claude_desktop":"running","cpu_pct":0.1,"rss_mb":13.1},
          "summary":{"active":2,"working":1,"waiting":1,"idle":0,"finished":3,"error":0},
          "sessions":[
            {"sid":"aaa-111","short":"aaa-111","title":"Перша задача","title_src":"custom",
             "project":"Alpha","branch":"main","state":"working",
             "activity":{"action":"edit_file","target":"src/a.cpp"},
             "started_at":1000,"last_update":2000,"seq":1040,"events":[]},
            {"sid":"bbb-222","short":"bbb-222","title":"Друга задача","title_src":"prompt",
             "project":"Beta","state":"waiting",
             "started_at":1500,"last_update":2500,"seq":1042,"events":[]}
          ],
          "finished":[
            {"sid":"ccc-333","short":"ccc-333","title":"Стара","project":"Gamma",
             "state":"finished","started_at":10,"last_update":900}
          ]
        }
        """.trimIndent()

        val snapshot = Protocol.parseSnapshot(json)
        assertNotNull(snapshot)
        assertEquals(2, snapshot!!.sessions.size)
        assertEquals(1, snapshot.finished.size)

        val first = snapshot.sessions[0]
        assertEquals("aaa-111", first.sessionId)
        assertEquals("Перша задача", first.title)
        assertEquals(Protocol.TitleSource.CUSTOM, first.titleSource)
        assertEquals(Protocol.ClaudeState.WORKING, first.state)
        assertEquals("edit_file", first.activity?.action)
        assertEquals("src/a.cpp", first.activity?.target)
        assertEquals("main", first.branch)

        val second = snapshot.sessions[1]
        assertEquals(Protocol.ClaudeState.WAITING, second.state)
        assertEquals(Protocol.TitleSource.PROMPT, second.titleSource)
        assertNull(second.branch)

        assertEquals(13.1, snapshot.bridge!!.rssMb, 0.001)
        assertEquals(1, snapshot.summary.working)
    }

    @Test
    fun `задача без ідентифікатора відкидається`() {
        val json = """
        {"t":"snapshot","seq":1,"sessions":[
          {"sid":"","title":"Без ідентифікатора","state":"working"},
          {"title":"Теж без","state":"working"},
          {"sid":"ok-1","title":"Справжня","state":"idle"}
        ]}
        """.trimIndent()

        val snapshot = Protocol.parseSnapshot(json)
        assertNotNull(snapshot)
        // Задачу без ідентифікатора неможливо показати коректно,
        // а вигадувати ідентифікатор неприпустимо.
        assertEquals(1, snapshot!!.sessions.size)
        assertEquals("ok-1", snapshot.sessions[0].sessionId)
    }

    @Test
    fun `кількість задач обмежена`() {
        val sessions = (1..50).joinToString(",") {
            """{"sid":"s-$it","title":"Задача $it","state":"working"}"""
        }
        val snapshot = Protocol.parseSnapshot("""{"t":"snapshot","seq":1,"sessions":[$sessions]}""")

        assertNotNull(snapshot)
        assertTrue(snapshot!!.sessions.size <= Protocol.MAX_SESSIONS)
    }

    @Test
    fun `невідомий стан стає UNKNOWN а не вигаданим`() {
        val json = """{"t":"snapshot","seq":1,"sessions":[
            {"sid":"a","title":"Т","state":"щось_нове_у_майбутній_версії"}]}"""
        val snapshot = Protocol.parseSnapshot(json)

        assertEquals(Protocol.ClaudeState.UNKNOWN, snapshot!!.sessions[0].state)
    }

    // ── Розбір подій ─────────────────────────────────────────────────────────

    @Test
    fun `події розбираються з прив'язкою до задач`() {
        val json = """
        {"t":"events","items":[
          {"sid":"aaa","seq":1,"ts":1000,"k":"activity","action":"edit_file","target":"a.cpp"},
          {"sid":"bbb","seq":2,"ts":1001,"k":"status","state":"waiting","text":"дозвольте"},
          {"sid":"aaa","seq":3,"ts":1002,"k":"result","status":"error","text":"збій"}
        ]}
        """.trimIndent()

        val events = Protocol.parseEventsMessage(json)
        assertNotNull(events)
        assertEquals(3, events!!.size)

        assertEquals("aaa", events[0].sessionId)
        assertEquals(Protocol.EventKind.ACTIVITY, events[0].kind)
        assertEquals("a.cpp", events[0].target)

        assertEquals("bbb", events[1].sessionId)
        assertEquals(Protocol.ClaudeState.WAITING, events[1].state)

        assertTrue(events[2].isError)
    }

    @Test
    fun `подія без ідентифікатора задачі відкидається`() {
        val json = """
        {"t":"events","items":[
          {"seq":1,"ts":1000,"k":"activity","action":"edit_file"},
          {"sid":"","seq":2,"ts":1001,"k":"output","text":"нікому"},
          {"sid":"ok","seq":3,"ts":1002,"k":"output","text":"справжня"}
        ]}
        """.trimIndent()

        val events = Protocol.parseEventsMessage(json)
        // Подію без задачі неможливо віднести нікуди, а приписувати її
        // «поточній» задачі не можна: такого поняття в моделі немає.
        assertEquals(1, events!!.size)
        assertEquals("ok", events[0].sessionId)
    }

    @Test
    fun `подія невідомого виду відкидається`() {
        val json = """{"t":"events","items":[
            {"sid":"a","seq":1,"ts":1,"k":"новий_вид_події"},
            {"sid":"a","seq":2,"ts":2,"k":"output","text":"нормальна"}]}"""

        val events = Protocol.parseEventsMessage(json)
        assertEquals(1, events!!.size)
    }

    @Test
    fun `повідомлення не того типу не розбирається як події`() {
        assertNull(Protocol.parseEventsMessage("""{"t":"snapshot","seq":1}"""))
        assertNull(Protocol.parseSnapshot("""{"t":"events","items":[]}"""))
    }

    // ── Формування кадрів ────────────────────────────────────────────────────

    @Test
    fun `ULID має правильну довжину й алфавіт`() {
        val ulid = Protocol.makeUlid()
        assertEquals(26, ulid.length)
        assertTrue(ulid.all { it in "0123456789ABCDEFGHJKMNPQRSTVWXYZ" })
    }

    @Test
    fun `ULID унікальні`() {
        val ids = (1..500).map { Protocol.makeUlid() }.toSet()
        assertEquals(500, ids.size)
    }

    @Test
    fun `сформований кадр проходить власну перевірку`() {
        val raw = Protocol.helloFrame("a".repeat(64))
        val frame = Protocol.parseFrame(raw)

        assertNotNull(frame)
        assertEquals("hello", frame!!.type)
        assertEquals("monitor", frame.payload.optString("role"))
    }

    @Test
    fun `кадр заявки на підключення має порожній ідентифікатор пропозиції`() {
        val raw = Protocol.pairClaimFrame("sig", "ecdh", "confirm")
        val frame = Protocol.parseFrame(raw)

        // Користувач знає лише код, тож ідентифікатор пропозиції порожній:
        // Relay розішле заявку всім активним пропозиціям.
        assertEquals("", frame!!.payload.optString("offer_id"))
        assertEquals("sig", frame.payload.optString("pub_sig"))
    }
}
