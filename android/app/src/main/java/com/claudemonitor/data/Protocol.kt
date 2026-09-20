package com.claudemonitor.data

import com.claudemonitor.i18n.Strings

import org.json.JSONArray
import org.json.JSONObject

/**
 * Моделі та розбір протоколу Claude Monitor v1.
 *
 * JSON обробляється вбудованим org.json: він є в складі Android і не додає
 * ані байта до APK, на відміну від зовнішніх бібліотек серіалізації
 * (Частина 4 §30, Частина 6 §39 Master Prompt).
 *
 * Увесь вхід вважається ворожим: жоден метод не кидає винятків назовні
 * і не виділяє пам'яті без обмежень.
 */
object Protocol {

    const val VERSION = 1

    // Ліміти мають збігатися з docs/protocol.md §2.
    const val MAX_FRAME_BYTES = 65536
    const val MAX_SESSIONS = 16
    // Межі історії подій на телефоні задає EventHistory: там і кількість,
    // і сумарний обсяг тексту.
    const val MAX_FINISHED = 20

    /** Скільки вузлів і рядків у картці приймається. Ті самі межі, що й у Bridge. */
    const val MAX_NODES = 16
    const val MAX_NODE_LINES = 12
    const val CLOCK_SKEW_MS = 300_000L

    // ── Стани ────────────────────────────────────────────────────────────────

    enum class ClaudeState(val wire: String) {
        UNKNOWN("unknown"),
        STARTING("starting"),
        IDLE("idle"),
        WORKING("working"),
        WAITING("waiting"),
        ERROR("error"),
        /** Вичерпано ліміт використання: Claude продовжить лише після скидання. */
        LIMITED("limited"),
        FINISHED("finished"),
        GONE("gone");

        companion object {
            fun from(value: String?): ClaudeState =
                entries.firstOrNull { it.wire == value } ?: UNKNOWN
        }
    }

    /** Джерело назви задачі — щоб UI не видавав здогадку за факт. */
    enum class TitleSource(val wire: String) {
        NONE("none"),
        CUSTOM("custom"),
        DERIVED("derived"),
        PROMPT("prompt"),
        PATH("path");

        companion object {
            fun from(value: String?): TitleSource =
                entries.firstOrNull { it.wire == value } ?: NONE
        }
    }

    enum class EventKind(val wire: String) {
        STATUS("status"),
        ACTIVITY("activity"),
        OUTPUT("output"),
        RESULT("result"),
        SESSION("session"),
        /** Повідомлення користувача — друга половина розмови. */
        PROMPT("prompt"),

        /** Рядок від вузла: «Натискаю: Файл». Пише мод, а не Claude. */
        NODE("node");

        companion object {
            fun from(value: String?): EventKind? = entries.firstOrNull { it.wire == value }
        }
    }

    // ── Дані ─────────────────────────────────────────────────────────────────

    data class Activity(val action: String, val target: String?)

    data class MonitorEvent(
        /** Належність до задачі. Подія без цього поля відкидається. */
        val sessionId: String,
        val sequence: Long,
        val timestampMs: Long,
        val kind: EventKind,
        val state: ClaudeState? = null,
        val action: String? = null,
        val target: String? = null,
        val text: String? = null,
        val isError: Boolean = false,

        /**
         * Назва вузла, який написав цей рядок (лише для [EventKind.NODE]).
         *
         * У чаті має бути видно, що рядок надійшов від мода, а не від Claude.
         */
        val nodeName: String? = null,
        /** Спільний номер частин однієї довгої репліки; 0 — репліка не ділилась. */
        val groupId: Long = 0,
        /** Номер частини (у склеєній репліці — останньої отриманої). */
        val part: Int = 0,
        val partCount: Int = 1,

        /**
         * Скільки зображень надіслали разом із запитом.
         *
         * Самі зображення на телефон не передаються — лише їхня кількість:
         * інакше запит «подивись на фото» виглядав би порожнім.
         */
        val imageCount: Int = 0,
        /** Коли почалась поточна відповідь Claude (у status посеред ходу). 0 — невідомо. */
        val turnStartedAtMs: Long = 0,
        /** Скільки тривала відповідь (у status «очікує» наприкінці ходу). 0 — невідомо. */
        val turnMs: Long = 0,
        /** Найбільший номер послідовності серед склеєних частин. */
        val lastSequence: Long = sequence,
        /**
         * Ідентифікатор запису на телефоні — ключ для списку в UI.
         * Номер послідовності для цього не годиться: після перезапуску Bridge
         * він починається заново, і ключі повторилися б.
         */
        val localId: Long = 0,
    )

    data class TaskSession(
        val sessionId: String,
        val short: String,
        val title: String,
        val titleSource: TitleSource,
        val project: String,
        /** Повний шлях: назви каталогів часто схожі й самі по собі плутають. */
        val cwd: String?,
        val branch: String?,
        val state: ClaudeState,
        val activity: Activity?,
        val startedAtMs: Long,
        val lastUpdateMs: Long,
        val sequence: Long,
        /** Скільки токенів займає контекст розмови. 0 — даних немає. */
        val contextTokens: Long = 0,
        val model: String? = null,
        /** Коли почалась поточна відповідь Claude. 0 — не триває або початок невідомий. */
        val turnStartedAtMs: Long = 0,
        /** Скільки тривала остання завершена відповідь. 0 — даних немає. */
        val lastTurnMs: Long = 0,
        val recentEvents: List<MonitorEvent> = emptyList(),
    ) {
        /**
         * Межа контексту моделі.
         *
         * Величини відомі публічно; для невідомої моделі повертається null,
         * і застосунок показує лише абсолютне число без відсотка — краще
         * не показати частку, ніж порахувати її від вигаданої межі.
         */
        val contextLimit: Long?
            get() {
                val assumed = when {
                    model == null -> return null
                    // Моделі з великим вікном. opus-4-8 сюди теж належить:
                    // спостережений контекст 229k перевищує 200k, отже
                    // припущення про менше вікно було б хибним.
                    model.contains("opus-5") ||
                        model.contains("sonnet-5") ||
                        model.contains("opus-4-8") -> 1_000_000L

                    model.contains("opus") ||
                        model.contains("sonnet") ||
                        model.contains("haiku") -> 200_000L

                    else -> return null
                }

                // Якщо фактичний контекст більший за припущену межу, значить
                // припущення неправильне. Краще не показати частку взагалі,
                // ніж показати «100%» там, де запас іще є.
                return if (contextTokens > assumed) null else assumed
            }

        val contextPercent: Int?
            get() {
                val limit = contextLimit ?: return null
                if (contextTokens <= 0) return null
                return ((contextTokens * 100) / limit).toInt().coerceIn(0, 100)
            }
    }

    data class FinishedTask(
        val sessionId: String,
        val short: String,
        val title: String,
        val project: String,
        val state: ClaudeState,
        val startedAtMs: Long,
        val lastUpdateMs: Long,
    )

    data class BridgeInfo(
        val host: String,
        val version: String,
        val uptimeSec: Long,
        val claudeDesktopRunning: Boolean,
        val cpuPercent: Double,
        val rssMb: Double,
    )

    data class Summary(
        val active: Int = 0,
        val working: Int = 0,
        val waiting: Int = 0,
        val idle: Int = 0,
        val finished: Int = 0,
        val error: Int = 0,
    )

    /** Рядок картки вузла: підпис і значення. */
    data class NodeLine(val label: String, val value: String)

    /**
     * Чому вузол не дав даних.
     *
     * З ноутбука приходить код, а не готовий текст: пояснення застосунок
     * пише своєю мовою.
     */
    enum class NodeError(val wire: String) {
        NONE(""),
        START_FAILED("start_failed"),
        TIMEOUT("timeout"),
        EXIT_CODE("exit_code"),
        BAD_OUTPUT("bad_output"),
        TOO_LARGE("too_large");

        companion object {
            fun from(value: String): NodeError = entries.firstOrNull { it.wire == value } ?: NONE
        }
    }

    /**
     * Картка вузла — розширення, яке власник ноутбука сам поклав у папку
     * `nodes`. Це додаткові дані поруч із задачами, а не частина Claude Code.
     */
    data class NodeCard(
        val id: String,
        val name: String,
        val status: String,
        val lines: List<NodeLine> = emptyList(),
        val text: String? = null,
        val error: NodeError = NodeError.NONE,
        val updatedAtMs: Long = 0,
    )

    data class Snapshot(
        val sequence: Long,
        val bridge: BridgeInfo?,
        val summary: Summary,
        val sessions: List<TaskSession>,
        val finished: List<FinishedTask>,
        val nodes: List<NodeCard> = emptyList(),
    )

    // ── Формування кадрів ────────────────────────────────────────────────────

    private const val CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

    fun makeUlid(): String {
        val out = StringBuilder(26)
        var time = System.currentTimeMillis()
        val timePart = CharArray(10)
        for (i in 9 downTo 0) {
            timePart[i] = CROCKFORD[(time % 32).toInt()]
            time /= 32
        }
        out.append(timePart)

        val random = Crypto.randomBytes(16)
        for (byte in random) out.append(CROCKFORD[byte.toInt() and 31])
        return out.toString()
    }

    fun frame(type: String, payload: JSONObject = JSONObject()): String =
        JSONObject().apply {
            put("v", VERSION)
            put("t", type)
            put("id", makeUlid())
            put("ts", System.currentTimeMillis())
            put("p", payload)
        }.toString()

    fun helloFrame(deviceId: String): String =
        frame("hello", JSONObject().apply {
            put("role", "monitor")
            put("device_id", deviceId)
            put("client", "claude-monitor-android/1.0.0")
        })

    fun authFrame(deviceId: String, signatureB64: String): String =
        frame("auth", JSONObject().apply {
            put("device_id", deviceId)
            put("sig", signatureB64)
        })

    fun pairClaimFrame(pubSig: String, pubEcdh: String, confirm: String): String =
        frame("pair_claim", JSONObject().apply {
            // Порожній offer_id: користувач знає лише код. Relay розішле
            // заявку всім активним пропозиціям, а підтвердить її той Bridge,
            // у якого збігся код (docs/protocol.md §4).
            put("offer_id", "")
            put("pub_sig", pubSig)
            put("pub_ecdh", pubEcdh)
            put("confirm", confirm)
        })

    fun forwardFrame(nonceB64: String, ciphertextB64: String): String =
        frame("fwd", JSONObject().apply {
            put("n", nonceB64)
            put("ct", ciphertextB64)
        })

    fun snapshotRequestJson(sinceSeq: Long): String =
        JSONObject().apply {
            put("t", "snapshot_request")
            put("since_seq", sinceSeq)
        }.toString()

    // ── Розбір ───────────────────────────────────────────────────────────────

    data class IncomingFrame(val type: String, val payload: JSONObject)

    /**
     * Розбирає кадр рівня 1.
     *
     * Порядок перевірок від найдешевших до найдорожчих, як у
     * docs/protocol.md §11: розмір -> JSON -> версія -> тип -> час.
     */
    fun parseFrame(raw: String, nowMs: Long = System.currentTimeMillis()): IncomingFrame? {
        if (raw.isEmpty() || raw.length > MAX_FRAME_BYTES) return null

        val root = runCatching { JSONObject(raw) }.getOrNull() ?: return null
        if (root.optInt("v", -1) != VERSION) return null

        val type = root.optString("t")
        if (type.isEmpty()) return null

        val timestamp = root.optLong("ts", 0)
        if (timestamp <= 0) return null
        // Позначка часу поза вікном — або збій годинника, або спроба
        // повторного відтворення старого кадру.
        if (kotlin.math.abs(timestamp - nowMs) > CLOCK_SKEW_MS) return null

        return IncomingFrame(type, root.optJSONObject("p") ?: JSONObject())
    }

    fun parseSnapshot(json: String): Snapshot? = runCatching {
        val root = JSONObject(json)
        if (root.optString("t") != "snapshot") return null

        val bridgeJson = root.optJSONObject("bridge")
        val bridge = bridgeJson?.let {
            BridgeInfo(
                host = it.optString("host", "?"),
                version = it.optString("version", "?"),
                uptimeSec = it.optLong("uptime_sec", 0),
                claudeDesktopRunning = it.optString("claude_desktop") == "running",
                cpuPercent = it.optDouble("cpu_pct", 0.0),
                rssMb = it.optDouble("rss_mb", 0.0),
            )
        }

        val summaryJson = root.optJSONObject("summary")
        val summary = if (summaryJson == null) Summary() else Summary(
            active = summaryJson.optInt("active"),
            working = summaryJson.optInt("working"),
            waiting = summaryJson.optInt("waiting"),
            idle = summaryJson.optInt("idle"),
            finished = summaryJson.optInt("finished"),
            error = summaryJson.optInt("error"),
        )

        Snapshot(
            sequence = root.optLong("seq", 0),
            bridge = bridge,
            summary = summary,
            sessions = parseSessions(root.optJSONArray("sessions")),
            finished = parseFinished(root.optJSONArray("finished")),
            nodes = parseNodes(root.optJSONArray("nodes")),
        )
    }.getOrNull()

    /**
     * Картки вузлів. Вузол без ідентифікатора відкидається — як і задача:
     * показати його коректно неможливо, а вигадувати нічого не можна.
     */
    private fun parseNodes(array: JSONArray?): List<NodeCard> {
        if (array == null) return emptyList()
        val out = ArrayList<NodeCard>(minOf(array.length(), MAX_NODES))

        for (i in 0 until minOf(array.length(), MAX_NODES)) {
            val item = array.optJSONObject(i) ?: continue
            val id = item.optString("id")
            if (id.isEmpty()) continue

            val linesJson = item.optJSONArray("lines")
            val count = minOf(linesJson?.length() ?: 0, MAX_NODE_LINES)
            val lines = ArrayList<NodeLine>(count)
            for (j in 0 until count) {
                val line = linesJson?.optJSONObject(j) ?: continue
                val label = line.optString("label")
                val value = line.optString("value")
                if (label.isEmpty() && value.isEmpty()) continue
                lines.add(NodeLine(label, value))
            }

            out.add(
                NodeCard(
                    id = id,
                    name = item.optString("name").ifEmpty { id },
                    status = item.optString("status").ifEmpty { "ok" },
                    lines = lines,
                    text = item.optString("text").ifEmpty { null },
                    error = NodeError.from(item.optString("error")),
                    updatedAtMs = item.optLong("updated_at", 0),
                )
            )
        }
        return out
    }

    private fun parseSessions(array: JSONArray?): List<TaskSession> {
        if (array == null) return emptyList()
        val out = ArrayList<TaskSession>(minOf(array.length(), MAX_SESSIONS))

        for (i in 0 until minOf(array.length(), MAX_SESSIONS)) {
            val item = array.optJSONObject(i) ?: continue
            val sessionId = item.optString("sid")
            // Задача без ідентифікатора не існує: показати її було б
            // неможливо коректно, а вигадати ідентифікатор — неприпустимо.
            if (sessionId.isEmpty()) continue

            val activityJson = item.optJSONObject("activity")
            val activity = activityJson?.let {
                Activity(
                    action = it.optString("action", "other"),
                    target = it.optString("target").ifEmpty { null },
                )
            }

            out.add(
                TaskSession(
                    sessionId = sessionId,
                    short = item.optString("short").ifEmpty { sessionId.take(8) },
                    title = item.optString("title").ifEmpty { sessionId.take(8) },
                    titleSource = TitleSource.from(item.optString("title_src")),
                    project = item.optString("project"),
                    cwd = item.optString("cwd").ifEmpty { null },
                    branch = item.optString("branch").ifEmpty { null },
                    state = ClaudeState.from(item.optString("state")),
                    activity = activity,
                    startedAtMs = item.optLong("started_at", 0),
                    lastUpdateMs = item.optLong("last_update", 0),
                    sequence = item.optLong("seq", 0),
                    contextTokens = item.optLong("context_tokens", 0),
                    turnStartedAtMs = item.optLong("turn_started_at", 0),
                    lastTurnMs = item.optLong("last_turn_ms", 0),
                    model = item.optString("model").ifEmpty { null },
                    recentEvents = parseEvents(item.optJSONArray("events")),
                )
            )
        }
        return out
    }

    private fun parseFinished(array: JSONArray?): List<FinishedTask> {
        if (array == null) return emptyList()
        val out = ArrayList<FinishedTask>(minOf(array.length(), MAX_FINISHED))

        for (i in 0 until minOf(array.length(), MAX_FINISHED)) {
            val item = array.optJSONObject(i) ?: continue
            val sessionId = item.optString("sid")
            if (sessionId.isEmpty()) continue

            out.add(
                FinishedTask(
                    sessionId = sessionId,
                    short = item.optString("short").ifEmpty { sessionId.take(8) },
                    title = item.optString("title").ifEmpty { sessionId.take(8) },
                    project = item.optString("project"),
                    state = ClaudeState.from(item.optString("state")),
                    startedAtMs = item.optLong("started_at", 0),
                    lastUpdateMs = item.optLong("last_update", 0),
                )
            )
        }
        return out
    }

    fun parseEventsMessage(json: String): List<MonitorEvent>? = runCatching {
        val root = JSONObject(json)
        if (root.optString("t") != "events") return null
        parseEvents(root.optJSONArray("items"))
    }.getOrNull()

    private fun parseEvents(array: JSONArray?): List<MonitorEvent> {
        if (array == null) return emptyList()
        val out = ArrayList<MonitorEvent>(array.length())

        for (i in 0 until array.length()) {
            val item = array.optJSONObject(i) ?: continue

            // Подія ЗАВЖДИ належить конкретній задачі. Без sid її неможливо
            // віднести нікуди — і приписувати «поточній» задачі не можна,
            // бо поняття поточної задачі в моделі немає.
            val sessionId = item.optString("sid")
            if (sessionId.isEmpty()) continue

            val kind = EventKind.from(item.optString("k")) ?: continue

            out.add(
                MonitorEvent(
                    sessionId = sessionId,
                    sequence = item.optLong("seq", 0),
                    timestampMs = item.optLong("ts", 0),
                    kind = kind,
                    state = item.optString("state").takeIf { it.isNotEmpty() }
                        ?.let { ClaudeState.from(it) },
                    action = item.optString("action").ifEmpty { null },
                    target = item.optString("target").ifEmpty { null },
                    text = item.optString("text").ifEmpty { null }
                        ?: item.optString("event").ifEmpty { null },
                    isError = item.optString("status") == "error",
                    nodeName = item.optString("node").ifEmpty { null },
                    groupId = item.optLong("gid", 0),
                    part = item.optInt("pt", 0),
                    partCount = item.optInt("pc", 1).coerceAtLeast(1),
                    imageCount = item.optInt("img", 0),
                    turnStartedAtMs = item.optLong("turn_start", 0),
                    turnMs = item.optLong("turn_ms", 0),
                )
            )
        }
        return out
    }

    // ── Pairing ──────────────────────────────────────────────────────────────

    // ── Перевірка адреси Relay ───────────────────────────────────────────────

    /**
     * Перевіряє адресу сервера.
     *
     * `wss://` дозволено завжди. `ws://` — ЛИШЕ для приватних адрес
     * локальної мережі: там немає інфраструктури для валідного сертифіката,
     * а трафік не залишає домашньої мережі. Для будь-якої публічної адреси
     * незашифрований транспорт відхиляється (Частина 3 §2, §23 Master Prompt).
     *
     * Вміст подій зашифрований наскрізно в обох випадках.
     *
     * @return null, якщо адреса прийнятна; інакше — пояснення для користувача
     */
    fun validateRelayUrl(raw: String, strings: Strings): String? {
        val url = raw.trim()
        if (url.isEmpty()) return strings.urlEmpty

        val isSecure = url.startsWith("wss://")
        val isPlain = url.startsWith("ws://")
        if (!isSecure && !isPlain) return strings.urlScheme

        val afterScheme = url.substringAfter("://")
        if (afterScheme.isEmpty()) return strings.urlInvalid

        val host = afterScheme.substringBefore('/').substringBefore(':')
        if (host.isEmpty()) return strings.urlInvalid

        if (isSecure) return null

        return if (isPrivateAddress(host)) {
            null
        } else {
            strings.urlPlainPublic
        }
    }

    /** Приватні діапазони RFC 1918 та локальний хост. */
    fun isPrivateAddress(host: String): Boolean {
        if (host == "localhost") return true

        val parts = host.split('.')
        if (parts.size != 4) return false

        val octets = parts.map { it.toIntOrNull() ?: return false }
        if (octets.any { it !in 0..255 }) return false

        return when {
            octets[0] == 127 -> true                                  // 127.0.0.0/8
            octets[0] == 10 -> true                                   // 10.0.0.0/8
            octets[0] == 192 && octets[1] == 168 -> true              // 192.168.0.0/16
            octets[0] == 172 && octets[1] in 16..31 -> true           // 172.16.0.0/12
            octets[0] == 169 && octets[1] == 254 -> true              // link-local
            else -> false
        }
    }

    /** Алфавіт коду: Base32 без символів, які легко сплутати. */
    private const val PAIRING_ALPHABET = "ABCDEFGHJKMNPQRSTVWXYZ0123456789"
    const val PAIRING_CODE_LENGTH = 12

    /**
     * Нормалізує введений код. Користувач переписує його з екрана,
     * тому плутанина I/1, O/0, U/V неминуча — виправляємо мовчки.
     */
    fun normalizePairingCode(input: String): String {
        val out = StringBuilder(PAIRING_CODE_LENGTH)
        for (raw in input) {
            if (raw == '-' || raw == ' ' || raw == '\t') continue
            var c = raw.uppercaseChar()
            when (c) {
                'I', 'L' -> c = '1'
                'O' -> c = '0'
                'U' -> c = 'V'
            }
            if (!PAIRING_ALPHABET.contains(c)) continue
            out.append(c)
            if (out.length >= PAIRING_CODE_LENGTH) break
        }
        return out.toString()
    }

    /**
     * Код підтвердження. Має точно збігатися з ComputeConfirm
     * у bridge/src/pairing.cpp, інакше pairing не відбудеться.
     */
    fun computeConfirm(code: String, role: String, pubSig: String, pubEcdh: String): String {
        val pairingKey = Crypto.hkdf(
            code.toByteArray(Charsets.UTF_8),
            "claude-monitor-v1/pairing".toByteArray(Charsets.UTF_8),
            ByteArray(0),
            32,
        )
        val message = (role + pubSig + pubEcdh).toByteArray(Charsets.UTF_8)
        return with(Crypto) { Crypto.hmacSha256(pairingKey, message).toBase64Url() }
    }
}
