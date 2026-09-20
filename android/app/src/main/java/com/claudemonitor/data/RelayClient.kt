package com.claudemonitor.data

import android.util.Log
import com.claudemonitor.data.Crypto.fromBase64Url
import com.claudemonitor.data.Crypto.toBase64Url
import com.claudemonitor.i18n.Strings
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.util.concurrent.TimeUnit
import kotlin.random.Random

/**
 * Клієнт Relay: TLS, автентифікація, наскрізне шифрування, перепідключення.
 *
 * Перевірка сертифіката НІКОДИ не вимикається. Жодного «довіряти всім
 * сертифікатам» тут немає і бути не може (Частина 3 §2 Master Prompt) —
 * OkHttp за замовчуванням перевіряє ланцюг довіри системним сховищем,
 * і цю поведінку тут не змінено.
 *
 * @param strings тексти поточної мови — беруться щоразу заново, бо мову
 *   можна змінити, поки з'єднання живе
 */
class RelayClient(
    private val identity: Identity,
    private val scope: CoroutineScope,
    private val strings: () -> Strings,
) {

    /** Стан з'єднання для показу користувачу. */
    enum class ConnectionState {
        DISCONNECTED,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        UNAUTHORIZED,
        REVOKED,
        PROTOCOL_MISMATCH,
    }

    interface Listener {
        fun onStateChanged(state: ConnectionState, detail: String? = null)
        fun onSnapshot(snapshot: Protocol.Snapshot)
        fun onEvents(events: List<Protocol.MonitorEvent>)
        fun onPeerPresence(online: Boolean)
    }

    var listener: Listener? = null

    private val client = OkHttpClient.Builder()
        // Стиснення й довгі таймаути тут не потрібні: кадри маленькі,
        // а мовчання розпізнається власним ping-механізмом.
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)  // WebSocket живе довго
        .writeTimeout(15, TimeUnit.SECONDS)
        // OkHttp сам надсилає ping-кадри рівня WebSocket: це тримає
        // з'єднання живим через NAT мобільного оператора.
        .pingInterval(45, TimeUnit.SECONDS)
        .retryOnConnectionFailure(false)  // перепідключенням керуємо самі
        .build()

    private val socketLock = Any()
    @Volatile private var webSocket: WebSocket? = null
    private var sessionKey: ByteArray? = null
    private var nonceCounter = Crypto.NonceCounter(DIRECTION_TO_BRIDGE)

    private val sendMutex = Mutex()
    private var attempt = 0
    @Volatile private var running = false
    private var lastSequence = 0L

    /**
     * Номер поточного циклу підключення.
     *
     * Без нього швидке «зупинити — запустити» (скажімо, після зміни адреси)
     * лишало живим старий цикл поруч із новим, і телефон тримав два з'єднання.
     */
    @Volatile private var generation = 0

    /**
     * Скільки повідомлень поспіль не вдалося розшифрувати.
     *
     * Одне таке повідомлення — випадковість, кілька поспіль — ознака, що ключі
     * розійшлися: найчастіше ноутбук спарили з іншим телефоном. Раніше телефон
     * відкидав їх мовчки й показував застарілі дані так, ніби все гаразд.
     */
    @Volatile private var undecryptableInARow = 0

    @Volatile private var state = ConnectionState.DISCONNECTED

    /** Чи запущено цикл підключення. */
    val isRunning: Boolean
        get() = running

    // ── Життєвий цикл ────────────────────────────────────────────────────────

    fun start() {
        if (running) return
        running = true
        undecryptableInARow = 0
        // Запуск після нового pairing: колишнє «відкликано» вже не стосується
        // нових ключів, і цикл не повинен на ньому зависати.
        if (state == ConnectionState.REVOKED) state = ConnectionState.DISCONNECTED
        val current = ++generation
        scope.launch(Dispatchers.IO) { connectLoop(current) }
    }

    fun stop() {
        running = false
        generation += 1
        synchronized(socketLock) {
            webSocket?.close(1000, "monitor stopped")
            webSocket = null
        }
        setState(ConnectionState.DISCONNECTED)
    }

    /** Примусове негайне перепідключення — наприклад, після зміни мережі. */
    fun reconnectNow() {
        attempt = 0
        synchronized(socketLock) {
            webSocket?.cancel()
            webSocket = null
        }
    }

    private suspend fun connectLoop(loop: Int) {
        fun alive() = running && loop == generation && scope.isActive

        while (alive()) {
            if (!identity.isPaired || identity.relayUrl.isEmpty()) {
                delay(2000)
                continue
            }

            // Стани, з яких немає сенсу повторювати спроби автоматично.
            if (state == ConnectionState.REVOKED || state == ConnectionState.PROTOCOL_MISMATCH) {
                delay(5000)
                continue
            }

            sessionKey = identity.sessionKey()
            if (sessionKey == null) {
                setState(ConnectionState.UNAUTHORIZED, strings().errKeyDerivation)
                delay(5000)
                continue
            }

            openConnection()

            // Чекаємо, доки з'єднання живе.
            while (alive() && webSocket != null) delay(250)

            if (!alive()) break

            val waitMs = backoffDelayMs()
            setState(ConnectionState.DISCONNECTED, strings().retryIn(waitMs / 1000))
            delay(waitMs)
        }
    }

    /**
     * Пауза перед наступною спробою: 1, 2, 4, 8, 16, 32, 60 секунд зі стелею
     * і випадковим відхиленням ±20%.
     *
     * Відхилення обов'язкове: без нього після падіння Relay усі клієнти
     * повернулися б синхронно й повалили б його знову (docs/protocol.md §10).
     */
    private fun backoffDelayMs(): Long {
        val base = minOf(1000L shl minOf(attempt, 5), MAX_BACKOFF_MS)
        attempt += 1
        val jitter = (base / 5).coerceAtLeast(1)
        return base - jitter + Random.nextLong(jitter * 2)
    }

    private fun openConnection() {
        val url = identity.relayUrl
        setState(ConnectionState.CONNECTING)

        val request = Request.Builder().url(url).build()

        // Під замком: інакше збій, що настав раніше за присвоєння, лишив би
        // у полі вже мертве з'єднання, і цикл чекав би на нього вічно.
        synchronized(socketLock) {
            webSocket = client.newWebSocket(request, object : WebSocketListener() {
                override fun onOpen(webSocket: WebSocket, response: Response) {
                    setState(ConnectionState.AUTHENTICATING)
                    nonceCounter = Crypto.NonceCounter(DIRECTION_TO_BRIDGE)
                    webSocket.send(Protocol.helloFrame(identity.deviceId))
                }

                override fun onMessage(webSocket: WebSocket, text: String) {
                    handleFrame(webSocket, text)
                }

                override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                    webSocket.close(1000, null)
                }

                override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                    // Зупинений клієнт свій стан уже оголосив — зокрема «відкликано»,
                    // яке не можна затерти звичайним «немає зв'язку».
                    if (release(webSocket) && running) setState(ConnectionState.DISCONNECTED)
                }

                override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                    // Назовні йде лише загальний опис: подробиці винятків
                    // користувачу не показуються (Частина 4 §22 Master Prompt).
                    Log.w(TAG, "з'єднання розірвано: ${t.javaClass.simpleName}")
                    if (release(webSocket) && running) {
                        setState(ConnectionState.DISCONNECTED, strings().noConnection)
                    }
                }
            })
        }
    }

    /** Забуває з'єднання, якщо воно досі поточне. Старі з'єднання стан не змінюють. */
    private fun release(socket: WebSocket): Boolean = synchronized(socketLock) {
        if (webSocket !== socket) return false
        webSocket = null
        true
    }

    // ── Обробка кадрів ───────────────────────────────────────────────────────

    private fun handleFrame(webSocket: WebSocket, raw: String) {
        val frame = Protocol.parseFrame(raw) ?: return

        when (frame.type) {
            "challenge" -> {
                val nonce = frame.payload.optString("nonce")
                if (nonce.isEmpty()) return

                val message = "claude-monitor-v1|auth|monitor|${identity.deviceId}|$nonce"
                val signed = runCatching { Crypto.sign(message) }
                val signature = signed.getOrNull()

                if (signature == null) {
                    // Причину показуємо користувачу: без неї стан виглядав
                    // просто як «немає звʼязку», хоча насправді проблема
                    // в підписі на самому пристрої.
                    val reason = signed.exceptionOrNull()?.let {
                        "${it.javaClass.simpleName}: ${it.message}"
                    } ?: strings().unknownReason
                    Log.e(TAG, "не вдалося підписати виклик: $reason")
                    setState(ConnectionState.UNAUTHORIZED, strings().signFailed(reason))
                    webSocket.cancel()
                    return
                }
                webSocket.send(Protocol.authFrame(identity.deviceId, signature.toBase64Url()))
            }

            "auth_ok" -> {
                // Лічильник скидається саме після успішної автентифікації:
                // інакше цикл «підключився — відмовили» став би потоком спроб.
                attempt = 0
                setState(ConnectionState.CONNECTED)
                listener?.onPeerPresence(frame.payload.optBoolean("peer_online", false))

                // Одразу просимо повний стан: без нього UI не має що показувати.
                sendEncrypted(webSocket, Protocol.snapshotRequestJson(lastSequence))
            }

            "fwd" -> {
                val key = sessionKey ?: return
                val nonce = frame.payload.optString("n").fromBase64Url() ?: return
                val ciphertext = frame.payload.optString("ct").fromBase64Url() ?: return

                val plaintext = Crypto.decrypt(key, nonce, ciphertext)
                if (plaintext == null) {
                    // Тег автентичності не зійшовся: повідомлення підроблене,
                    // пошкоджене — або зашифроване для іншого телефона.
                    Log.w(TAG, "не вдалося розшифрувати повідомлення")
                    onUndecryptable()
                    return
                }
                undecryptableInARow = 0
                handlePayload(webSocket, plaintext)
            }

            "error" -> handleError(webSocket, frame.payload.optString("code"))

            "ping" -> webSocket.send(Protocol.frame("pong"))
        }
    }

    /**
     * Повідомлення, яке не вдалося розшифрувати.
     *
     * Кілька поспіль означають, що ключі телефона й ноутбука розійшлися. Про це
     * кажемо прямо: інакше застосунок виглядав би підключеним, а дані в ньому
     * просто застигли б.
     */
    private fun onUndecryptable() {
        undecryptableInARow += 1
        if (undecryptableInARow >= KEYS_MISMATCH_THRESHOLD && state != ConnectionState.UNAUTHORIZED) {
            setState(ConnectionState.UNAUTHORIZED, strings().keysMismatch)
        }
    }

    private fun handlePayload(webSocket: WebSocket, json: String) {
        Protocol.parseSnapshot(json)?.let { snapshot ->
            lastSequence = snapshot.sequence
            listener?.onSnapshot(snapshot)
            return
        }

        Protocol.parseEventsMessage(json)?.let { events ->
            if (events.isEmpty()) return

            // Розрив у нумерації означає, що частина подій не дійшла.
            // Замість здогадок просимо повний стан заново
            // (docs/protocol.md §7).
            val firstSeq = events.first().sequence
            if (lastSequence > 0 && firstSeq > lastSequence + 1) {
                Log.i(TAG, "виявлено пропуск подій, запитую знімок")
                sendEncrypted(webSocket, Protocol.snapshotRequestJson(lastSequence))
            }

            lastSequence = maxOf(lastSequence, events.last().sequence)
            listener?.onEvents(events)
        }
    }

    private fun handleError(webSocket: WebSocket, code: String) {
        val text = strings()
        when (code) {
            "peer_online" -> listener?.onPeerPresence(true)
            "peer_offline" -> listener?.onPeerPresence(false)

            "device_revoked" -> {
                // Доступ відкликано: ноутбук спарили з іншим телефоном або цей
                // телефон відкликали вручну. Стираємо ключі й зупиняємо спроби:
                // повторювати їх марно, а стан «відкликано» має лишитися видимим
                // на екрані підключення.
                identity.forgetPeer(deleteKeys = true)
                running = false
                generation += 1
                setState(ConnectionState.REVOKED, text.revokedFromLaptop)
                webSocket.cancel()
            }

            "auth_failed" -> {
                setState(ConnectionState.UNAUTHORIZED, text.deviceNotAuthorized)
                webSocket.cancel()
            }

            "proto_unsupported" -> {
                // Мовчки інтерпретувати невідому версію не можна.
                setState(ConnectionState.PROTOCOL_MISMATCH, text.updateApp)
                webSocket.cancel()
            }

            "rate_limited" -> {
                setState(ConnectionState.DISCONNECTED, text.tooManyRequests)
                webSocket.cancel()
            }

            "not_paired" -> setState(ConnectionState.UNAUTHORIZED, text.laptopNotPaired)
        }
    }

    private fun sendEncrypted(webSocket: WebSocket, json: String) {
        val key = sessionKey ?: return
        scope.launch(Dispatchers.IO) {
            sendMutex.withLock {
                // Лічильник nonce має бути строго монотонним, тому доступ
                // до нього серіалізовано: повторний nonce розкрив би
                // відкритий текст.
                val nonce = nonceCounter.next()
                val ciphertext = runCatching { Crypto.encrypt(key, nonce, json) }.getOrNull()
                    ?: return@withLock
                webSocket.send(
                    Protocol.forwardFrame(nonce.toBase64Url(), ciphertext.toBase64Url())
                )
            }
        }
    }

    private fun setState(newState: ConnectionState, detail: String? = null) {
        state = newState
        listener?.onStateChanged(newState, detail)
    }

    // ── Pairing ──────────────────────────────────────────────────────────────

    /**
     * Одноразове підключення для pairing.
     *
     * @return null за успіху, інакше текст помилки для показу користувачу
     */
    suspend fun performPairing(relayUrl: String, rawCode: String): String? {
        val text = strings()
        val code = Protocol.normalizePairingCode(rawCode)
        if (code.length != Protocol.PAIRING_CODE_LENGTH) {
            return text.codeLength(Protocol.PAIRING_CODE_LENGTH)
        }

        identity.ensureKeys()

        val pubSig = identity.publicSign
        val pubEcdh = identity.publicExchange
        val confirm = Protocol.computeConfirm(code, "monitor", pubSig, pubEcdh)

        // Слід виконання: якщо щось піде не так, користувач має побачити,
        // на якому саме кроці, а не безпорадне «час вичерпано».
        val trace = StringBuilder()
        fun note(step: String) {
            Log.i(TAG, "pairing: $step")
            trace.append(step).append('\n')
        }

        note("старт")

        // Результат загорнуто в Result, а не передається як String?.
        //
        // Причина: withTimeoutOrNull повертає null при таймауті, а успіх
        // раніше теж позначався null. Ці два значення були нерозрізненні,
        // тож успішний pairing показувався користувачу як «час очікування
        // вичерпано» — попри те, що ключі вже збережено.
        val outcome: Result<String?>? = kotlinx.coroutines.withTimeoutOrNull(45_000) {
            kotlinx.coroutines.suspendCancellableCoroutine<Result<String?>> { continuation ->
                val request = Request.Builder().url(relayUrl).build()

                val socket = client.newWebSocket(request, object : WebSocketListener() {
                    override fun onOpen(webSocket: WebSocket, response: Response) {
                        note("зʼєднання відкрито (HTTP ${response.code})")
                        webSocket.send(Protocol.pairClaimFrame(pubSig, pubEcdh, confirm))
                        note("заявку надіслано")
                    }

                    override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                        note("сервер закрив зʼєднання: код $code")
                        finish(continuation, webSocket, text.serverClosed(code) + "\n\n$trace")
                    }

                    override fun onMessage(webSocket: WebSocket, text: String) {
                        handlePairingMessage(webSocket, text, continuation, relayUrl, trace, ::note)
                    }

                    override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                        note("збій: ${t.javaClass.simpleName} ${t.message ?: ""}")
                        finish(continuation, webSocket, text.noServerConnection + "\n\n$trace")
                    }
                })

                continuation.invokeOnCancellation { socket.cancel() }
            }
        }

        // null тут означає рівно одне — спрацював таймаут. Успіх приходить
        // як Result.success(null), невдача — як Result.success(текст).
        if (outcome == null) {
            return text.pairTimeout + "\n$trace"
        }
        return outcome.getOrNull()
    }

    private fun handlePairingMessage(
        webSocket: WebSocket,
        raw: String,
        continuation: kotlinx.coroutines.CancellableContinuation<Result<String?>>,
        relayUrl: String,
        trace: StringBuilder,
        note: (String) -> Unit,
    ) {
        val text = strings()
        val frame = Protocol.parseFrame(raw)
        if (frame == null) {
            // Найчастіша причина — розбіжність годинників:
            // кадр із «неправильним» часом відкидається як
            // можливе повторне відтворення.
            val type = runCatching { org.json.JSONObject(raw).optString("t") }.getOrNull() ?: "?"
            val ts = runCatching { org.json.JSONObject(raw).optLong("ts") }.getOrNull() ?: 0L
            val diff = (System.currentTimeMillis() - ts) / 1000
            note("кадр «$type» ВІДХИЛЕНО (розбіжність часу ${diff} с)")
            return
        }
        note("отримано «${frame.type}»")

        when (frame.type) {
            "pair_ok" -> {
                if (!frame.payload.optBoolean("accepted", false)) {
                    finish(continuation, webSocket, text.laptopRejected)
                    return
                }
                val peerSign = frame.payload.optString("peer_pub_sig")
                val peerEcdh = frame.payload.optString("peer_pub_ecdh")
                if (peerSign.isEmpty() || peerEcdh.isEmpty()) {
                    finish(continuation, webSocket, text.incompleteReply)
                    return
                }

                // Збереження ключів обгорнуте навмисно.
                //
                // OkHttp ковтає винятки, що виникають у цьому
                // зворотному виклику: процедура просто зависала
                // до таймауту, хоча підтвердження вже прийшло.
                // Тепер причина видно одразу.
                try {
                    note("зберігаю ключі")
                    identity.savePeer(peerSign, peerEcdh)
                    identity.relayUrl = relayUrl
                    note("ключі збережено")

                    // Перевіряємо, що спільний ключ шифрування
                    // справді обчислюється: інакше підключення
                    // все одно не запрацює, і краще дізнатися
                    // про це зараз.
                    if (identity.sessionKey() == null) {
                        finish(continuation, webSocket, text.keyDerivationFailed + "\n\n$trace")
                        return
                    }
                    note("ключ шифрування обчислено")
                    finish(continuation, webSocket, null)
                } catch (e: Throwable) {
                    note("ПОМИЛКА: ${e.javaClass.simpleName}: ${e.message}")
                    finish(continuation, webSocket, text.keysSaveFailed + "\n\n$trace")
                }
            }

            "error" -> {
                val message = when (frame.payload.optString("code")) {
                    "auth_failed" -> text.pairAuthFailed
                    "rate_limited" -> text.pairRateLimited
                    "peer_offline" -> text.pairPeerOffline
                    "proto_unsupported" -> text.pairProtoUnsupported
                    else -> text.pairFailed
                }
                finish(continuation, webSocket, message)
            }
        }
    }

    private fun finish(
        continuation: kotlinx.coroutines.CancellableContinuation<Result<String?>>,
        webSocket: WebSocket,
        result: String?,
    ) {
        webSocket.close(1000, null)
        if (continuation.isActive) {
            continuation.resumeWith(Result.success(Result.success(result)))
        }
    }

    private companion object {
        const val TAG = "RelayClient"
        const val DIRECTION_TO_BRIDGE = "M2B "
        const val MAX_BACKOFF_MS = 60_000L

        /** Скільки нерозшифрованих повідомлень поспіль означають розбіжність ключів. */
        const val KEYS_MISMATCH_THRESHOLD = 3
    }
}
