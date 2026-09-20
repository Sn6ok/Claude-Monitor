package com.claudemonitor

import android.app.Application
import android.app.Notification
import com.claudemonitor.data.AppSettings
import com.claudemonitor.data.EventHistory
import com.claudemonitor.data.Identity
import com.claudemonitor.data.NotificationRules
import com.claudemonitor.data.Protocol
import com.claudemonitor.data.RelayClient
import com.claudemonitor.data.SettingsStore
import com.claudemonitor.data.TaskFilter
import com.claudemonitor.data.TaskNotification
import com.claudemonitor.data.allows
import com.claudemonitor.data.updatedBy
import com.claudemonitor.i18n.Strings
import com.claudemonitor.i18n.stringsFor
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Що саме показує застосунок просто зараз. */
enum class AppScreen { PAIRING, TASK_LIST, TASK_DETAIL, SETTINGS }

data class UiState(
    val screen: AppScreen = AppScreen.PAIRING,
    val connection: RelayClient.ConnectionState = RelayClient.ConnectionState.DISCONNECTED,
    val connectionDetail: String? = null,
    val bridgeOnline: Boolean = false,

    val bridge: Protocol.BridgeInfo? = null,

    /** Картки вузлів — розширень, доданих на ноутбуці. */
    val nodes: List<Protocol.NodeCard> = emptyList(),

    /** Задачі за ідентифікатором. Порядок для показу дає [orderedTasks]. */
    val tasks: Map<String, Protocol.TaskSession> = emptyMap(),

    /** Події кожної задачі окремо — буфер обмеженого розміру. */
    val taskEvents: Map<String, List<Protocol.MonitorEvent>> = emptyMap(),

    /** Обрана вкладка головного екрана: всі, активні чи неактивні задачі. */
    val taskFilter: TaskFilter = TaskFilter.ALL,

    val selectedTaskId: String? = null,
    val lastUpdateMs: Long = 0,

    val pairingInProgress: Boolean = false,
    val pairingError: String? = null,
    val relayUrlInput: String = "",
    val codeInput: String = "",

    val settings: AppSettings = AppSettings(),
) {
    /** Задачі у сталому порядку: спершу ті, що потребують уваги. */
    val orderedTasks: List<Protocol.TaskSession>
        get() = tasks.values.sortedWith(
            compareBy(
                { statePriority(it.state) },
                { it.startedAtMs },
            )
        )

    val selectedTask: Protocol.TaskSession?
        get() = selectedTaskId?.let { tasks[it] }

    /** Чи давно не було оновлень — щоб відрізнити спокій від обриву. */
    val isStale: Boolean
        get() = lastUpdateMs > 0 &&
            System.currentTimeMillis() - lastUpdateMs > STALE_THRESHOLD_MS

    private fun statePriority(state: Protocol.ClaudeState): Int = when (state) {
        Protocol.ClaudeState.ERROR -> 0
        Protocol.ClaudeState.WAITING -> 1
        Protocol.ClaudeState.WORKING -> 2
        Protocol.ClaudeState.STARTING -> 3
        Protocol.ClaudeState.LIMITED -> 4
        Protocol.ClaudeState.IDLE -> 5
        else -> 6
    }

    private companion object {
        /// Поріг, після якого дані вважаються застарілими.
        ///
        /// Він має бути помітно більшим за період, з яким Bridge оновлює
        /// стан у спокійні періоди (20 с). Інакше позначка спалахувала б
        /// щоразу між оновленнями — і означала б не втрату зв'язку,
        /// а просто відсутність активності в Claude Code.
        const val STALE_THRESHOLD_MS = 60_000L
    }
}

/**
 * Єдине джерело істини застосунку (Частина 4 §34 Master Prompt).
 *
 * Живе в [MonitorApp], а не в Activity: коли ввімкнені сповіщення, з'єднання
 * й стан мають пережити згортання застосунку.
 *
 * Ключове рішення для мультизадачності: стан кожної задачі лежить у
 * власному записі Map за ідентифікатором сесії. Глобального поля
 * «поточна задача» не існує — саме тому дві задачі принципово не можуть
 * перезаписати стан одна одної (docs/architecture.md §5.1).
 */
class MonitorController(private val app: Application) {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val identity = Identity(app)
    private val settingsStore = SettingsStore(app)
    private val notifier = Notifier(app)

    private val _state = MutableStateFlow(UiState(settings = settingsStore.load()))
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** Тексти поточної мови. */
    val strings: Strings
        get() = stringsFor(_state.value.settings.language)

    private val client = RelayClient(identity, scope) { strings }

    /** Чи застосунок зараз на екрані. Читається з потоку мережі, тому @Volatile. */
    @Volatile private var uiVisible = false
    private var serviceRunning = false

    /** Задача, яку треба відкрити після дотику до сповіщення, щойно вона стане відома. */
    @Volatile private var pendingTaskId: String? = null

    /** Зміна стану, про яку, можливо, варто сповістити. */
    private class Alert(
        val task: Protocol.TaskSession,
        val kind: TaskNotification,
        val note: String?,
        val turnMs: Long,
    )

    init {
        _state.update {
            it.copy(
                screen = if (identity.isPaired) AppScreen.TASK_LIST else AppScreen.PAIRING,
                relayUrlInput = identity.relayUrl,
            )
        }
        notifier.ensureChannels(strings)

        client.listener = object : RelayClient.Listener {
            override fun onStateChanged(state: RelayClient.ConnectionState, detail: String?) {
                _state.update { current ->
                    // Відкликання доступу повертає на екран підключення:
                    // без пари показувати нема чого.
                    val screen = if (state == RelayClient.ConnectionState.REVOKED) {
                        AppScreen.PAIRING
                    } else {
                        current.screen
                    }
                    current.copy(connection = state, connectionDetail = detail, screen = screen)
                }
                // Без пари фоновому з'єднанню й сервісу працювати нема для чого.
                if (state == RelayClient.ConnectionState.REVOKED) {
                    scope.launch { updateConnection() }
                }
            }

            override fun onSnapshot(snapshot: Protocol.Snapshot) = applySnapshot(snapshot)
            override fun onEvents(events: List<Protocol.MonitorEvent>) = applyEvents(events)

            override fun onPeerPresence(online: Boolean) {
                _state.update { it.copy(bridgeOnline = online) }
            }
        }

        updateConnection()
    }

    // ── Оновлення стану ──────────────────────────────────────────────────────

    /**
     * Знімок повністю заміщує перелік задач.
     *
     * Задачі, яких у знімку немає, зникають разом зі своїми подіями — але
     * їхні дані НЕ переходять до інших задач: кожен запис прив'язаний
     * до власного ідентифікатора.
     */
    private fun applySnapshot(snapshot: Protocol.Snapshot) {
        var alerts: List<Alert> = emptyList()

        _state.update { current ->
            val tasks = snapshot.sessions.associateBy { it.sessionId }

            // Зміни стану, які могли пройти повз окремі події (скажімо, під час
            // обриву зв'язку). Та сама зміна, що вже прийшла подією, тут не
            // повториться: стан у знімку з ним уже збігається.
            val found = ArrayList<Alert>()
            for (task in snapshot.sessions) {
                val before = current.tasks[task.sessionId] ?: continue
                NotificationRules.forTransition(before.state, task.state, null)?.let {
                    found += Alert(task, it, null, task.lastTurnMs)
                }
            }
            alerts = found

            // Знімок ДОПОВНЮЄ накопичену історію, а не замінює її.
            //
            // Bridge надсилає знімок раз на 20 секунд, і в ньому лише двадцять
            // останніх подій кожної задачі. Коли знімок замінював історію,
            // відповіді Claude зникали з екрана просто під час читання.
            val events = HashMap<String, List<Protocol.MonitorEvent>>()
            for (session in snapshot.sessions) {
                events[session.sessionId] = EventHistory.mergeSnapshot(
                    current.taskEvents[session.sessionId].orEmpty(),
                    session.recentEvents,
                )
            }

            // Задача зі сповіщення відкривається, щойно про неї стало відомо.
            val pending = pendingTaskId?.takeIf { tasks.containsKey(it) }

            // Якщо відкрита задача зникла, повертаємось до списку, а не
            // показуємо чужі дані під її назвою.
            val selected = pending ?: current.selectedTaskId?.takeIf { tasks.containsKey(it) }
            val screen = when {
                pending != null -> AppScreen.TASK_DETAIL
                current.screen == AppScreen.TASK_DETAIL && selected == null -> AppScreen.TASK_LIST
                else -> current.screen
            }

            current.copy(
                bridge = snapshot.bridge,
                nodes = snapshot.nodes,
                tasks = tasks,
                taskEvents = events,
                selectedTaskId = selected,
                screen = screen,
                lastUpdateMs = System.currentTimeMillis(),
            )
        }

        if (pendingTaskId != null && pendingTaskId == _state.value.selectedTaskId) {
            pendingTaskId = null
        }
        postAlerts(alerts)
    }

    /**
     * Застосовує пакет подій.
     *
     * Подія з невідомим ідентифікатором задачі ВІДКИДАЄТЬСЯ. Приписати її
     * якійсь наявній задачі було б гірше за втрату: користувач побачив би
     * чужу активність під не тією назвою.
     */
    private fun applyEvents(events: List<Protocol.MonitorEvent>) {
        var alerts: List<Alert> = emptyList()

        _state.update { current ->
            val tasks = current.tasks.toMutableMap()
            val taskEvents = current.taskEvents.toMutableMap()
            val found = ArrayList<Alert>()

            for (event in events) {
                val task = tasks[event.sessionId]
                if (task == null && event.kind != Protocol.EventKind.SESSION) {
                    // Задача ще не відома — подія чекатиме наступного знімка.
                    continue
                }

                if (task != null) {
                    // Правила оновлення картки, зокрема таймера відповіді, — в updatedBy.
                    val updated = task.updatedBy(event)
                    tasks[event.sessionId] = updated

                    NotificationRules.forTransition(task.state, updated.state, event.text)?.let {
                        found += Alert(updated, it, event.text, event.turnMs)
                    }
                }

                // Історія лише доповнюється. Межі памʼяті та склеювання частин
                // довгої репліки — в EventHistory (Частина 4 §21 Master Prompt).
                taskEvents[event.sessionId] =
                    EventHistory.append(taskEvents[event.sessionId].orEmpty(), event)
            }
            alerts = found

            current.copy(
                tasks = tasks,
                taskEvents = taskEvents,
                lastUpdateMs = System.currentTimeMillis(),
            )
        }

        postAlerts(alerts)
    }

    /**
     * Показує сповіщення про зміни стану.
     *
     * Лише коли застосунок згорнуто: на відкритому екрані зміну й так видно,
     * а спливаюче вікно поверх нього тільки заважало б.
     */
    private fun postAlerts(alerts: List<Alert>) {
        if (alerts.isEmpty() || uiVisible) return
        val settings = _state.value.settings
        val text = strings
        for (alert in alerts) {
            if (settings.allows(alert.kind)) {
                notifier.showTask(alert.task, alert.kind, alert.note, alert.turnMs, text)
            }
        }
    }

    // ── Дії користувача ──────────────────────────────────────────────────────

    fun openTask(sessionId: String) {
        _state.update { it.copy(selectedTaskId = sessionId, screen = AppScreen.TASK_DETAIL) }
    }

    /** Дотик до сповіщення: задачу відкриваємо одразу або щойно прийде знімок. */
    fun openTaskFromNotification(sessionId: String) {
        if (!identity.isPaired) return
        if (_state.value.tasks.containsKey(sessionId)) {
            pendingTaskId = null
            openTask(sessionId)
        } else {
            pendingTaskId = sessionId
        }
    }

    fun showTaskList() {
        _state.update { it.copy(screen = AppScreen.TASK_LIST, selectedTaskId = null) }
    }

    fun setTaskFilter(filter: TaskFilter) = _state.update { it.copy(taskFilter = filter) }
    fun showSettings() = _state.update { it.copy(screen = AppScreen.SETTINGS) }

    fun onRelayUrlChanged(value: String) = _state.update { it.copy(relayUrlInput = value) }
    fun onCodeChanged(value: String) = _state.update { it.copy(codeInput = value, pairingError = null) }

    /**
     * Змінює налаштування й одразу застосовує їх.
     *
     * Тема й мова змінюються без перезапуску; увімкнення сповіщень запускає
     * фонове з'єднання, вимкнення — зупиняє його.
     */
    fun updateSettings(transform: (AppSettings) -> AppSettings) {
        val previous = _state.value.settings
        val updated = transform(previous)
        if (updated == previous) return

        settingsStore.save(updated)
        _state.update { it.copy(settings = updated) }

        if (updated.language != previous.language) {
            notifier.ensureChannels(stringsFor(updated.language))
            // Повторний запуск сервісу оновлює текст його сповіщення новою мовою.
            if (serviceRunning) MonitorService.start(app)
        }
        if (updated.notificationsEnabled != previous.notificationsEnabled) {
            updateConnection()
        }
    }

    /**
     * Змінює адресу сервера, не розриваючи вже встановлену довіру.
     *
     * Це потрібно частіше, ніж здається: безкоштовні тунелі видають нову
     * адресу за кожного запуску. Без цієї можливості довелося б щоразу
     * проходити pairing заново, хоча ключі лишаються ті самі.
     */
    fun applyRelayUrl() {
        val url = _state.value.relayUrlInput.trim()

        val error = Protocol.validateRelayUrl(url, strings)
        if (error != null) {
            _state.update { it.copy(pairingError = error) }
            return
        }

        identity.relayUrl = url
        _state.update { it.copy(pairingError = null, connectionDetail = strings.relayUpdated) }

        // Перепідключення до нового сервера з тими самими ключами.
        client.stop()
        updateConnection()
        client.reconnectNow()
    }

    fun pair() {
        val current = _state.value
        if (current.pairingInProgress) return

        val url = current.relayUrlInput.trim()
        val urlError = Protocol.validateRelayUrl(url, strings)
        if (urlError != null) {
            _state.update { it.copy(pairingError = urlError) }
            return
        }

        _state.update { it.copy(pairingInProgress = true, pairingError = null) }

        scope.launch {
            val error = client.performPairing(url, current.codeInput)
            if (error == null) {
                _state.update {
                    it.copy(
                        pairingInProgress = false,
                        pairingError = null,
                        codeInput = "",
                        screen = AppScreen.TASK_LIST,
                    )
                }
                updateConnection()
            } else {
                _state.update { it.copy(pairingInProgress = false, pairingError = error) }
            }
        }
    }

    fun unpair() {
        client.stop()
        identity.forgetPeer(deleteKeys = true)
        _state.update {
            UiState(
                screen = AppScreen.PAIRING,
                relayUrlInput = identity.relayUrl,
                settings = it.settings,
            )
        }
        updateConnection()
    }

    fun reconnect() = client.reconnectNow()

    // ── Життєвий цикл ────────────────────────────────────────────────────────

    /** Застосунок з'явився на екрані. */
    fun onUiVisible() {
        uiVisible = true
        updateConnection()
    }

    /** Застосунок згорнуто. */
    fun onUiHidden() {
        uiVisible = false
        updateConnection()
    }

    fun backgroundNotification(): Notification = notifier.backgroundNotification(strings)

    /** Чи потрібне з'єднання у фоні: є пара, сповіщення ввімкнені й дозволені системою. */
    fun backgroundModeWanted(): Boolean =
        identity.isPaired &&
            _state.value.settings.notificationsEnabled &&
            canPostNotifications(app)

    fun onBackgroundServiceStarted() {
        serviceRunning = true
        updateConnection()
    }

    fun onBackgroundServiceStopped() {
        serviceRunning = false
        updateConnection()
    }

    /**
     * Узгоджує з'єднання й фоновий сервіс із поточною ситуацією.
     *
     * З'єднання живе, поки застосунок на екрані, а з увімкненими сповіщеннями —
     * ще й у фоні. Сервіс запускається лише з видимого застосунку: запуск
     * із фону Android забороняє.
     */
    private fun updateConnection() {
        val background = backgroundModeWanted()

        if (background && uiVisible && !serviceRunning) MonitorService.start(app)
        if (!background && serviceRunning) MonitorService.stop(app)

        val shouldRun = identity.isPaired && (uiVisible || (background && serviceRunning))
        if (shouldRun) {
            client.start()
        } else if (client.isRunning) {
            // З'єднання закривається свідомо: тримати сокет заради екрана,
            // якого ніхто не бачить, означало б марно витрачати батарею
            // (Частина 4 §12 Master Prompt).
            client.stop()
        }
    }
}
