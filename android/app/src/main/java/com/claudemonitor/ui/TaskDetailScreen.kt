package com.claudemonitor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.claudemonitor.UiState
import com.claudemonitor.data.Protocol
import com.claudemonitor.i18n.LocalStrings
import com.claudemonitor.i18n.Strings
import com.claudemonitor.ui.theme.ProgressTrack
import com.claudemonitor.ui.theme.StateError
import com.claudemonitor.ui.theme.StateIdle
import com.claudemonitor.ui.theme.StateWaiting
import com.claudemonitor.ui.theme.StateWorking
import kotlinx.coroutines.delay

/**
 * Детальний перегляд однієї задачі.
 *
 * Екран лише показує. Кнопок керування Claude Code тут немає й не буде:
 * у версії 1 застосунок строго read-only (Частина 4 §27 Master Prompt).
 * Копіювання тексту до керування не належить — воно нічого не змінює
 * на ноутбуці.
 */
@Composable
fun TaskDetailScreen(
    state: UiState,
    onBack: () -> Unit,
) {
    val strings = LocalStrings.current
    val task = state.selectedTask
    if (task == null) {
        // Задача зникла, доки екран був відкритий. Показуємо це прямо,
        // а не підставляємо дані іншої задачі.
        Column(
            Modifier.fillMaxSize().padding(24.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(strings.taskGone, style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(12.dp))
            Text(
                text = strings.backToList,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.clickable { onBack() },
            )
        }
        return
    }

    var nowMs by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(task.sessionId) {
        while (true) {
            delay(1000)
            nowMs = System.currentTimeMillis()
        }
    }

    val context = LocalContext.current
    val copy: (String) -> Unit = { text -> copyToClipboard(context, text, strings.copied) }

    // Події саме цієї задачі — беруться за її ідентифікатором, тож
    // чужа активність тут з'явитися не може.
    val events = state.taskEvents[task.sessionId].orEmpty()
    val listState = rememberLazyListState()

    // Стрічка читається згори вниз, тож новий запис зʼявляється внизу — як
    // у терміналі. Прокручуємо до нього, але лише коли користувач і так
    // унизу: якщо він гортає історію вгору, стрибок до кінця на кожну
    // нову подію не дав би дочитати. Довжина останнього тексту — теж ключ:
    // частина довгої репліки дописується до вже показаного повідомлення,
    // і кількість записів при цьому не змінюється.
    var scrolledOnce by remember(task.sessionId) { mutableStateOf(false) }
    val followBottom by remember {
        derivedStateOf {
            val info = listState.layoutInfo
            val last = info.visibleItemsInfo.lastOrNull()
            last == null || last.index >= info.totalItemsCount - 2
        }
    }
    val lastTextLength = events.lastOrNull()?.text?.length ?: 0
    LaunchedEffect(events.size, lastTextLength, task.sessionId) {
        if (events.isNotEmpty() && (!scrolledOnce || followBottom)) {
            listState.animateScrollToItem(events.size - 1)
            scrolledOnce = true
        }
    }

    Column(Modifier.fillMaxSize()) {

        DetailHeader(
            task = task,
            nowMs = nowMs,
            canCopyAll = events.isNotEmpty(),
            onBack = onBack,
            onCopyAll = { copy(chatAsText(task, events, strings)) },
        )

        // Дані не оновлюються — кажемо про це прямо в чаті, а не лише на
        // головному екрані: інакше застиглий чат виглядав би як спокій.
        val connected = state.connection == com.claudemonitor.data.RelayClient.ConnectionState.CONNECTED
        val warning = when {
            !connected -> listOfNotNull(connectionLabel(state.connection, strings), state.connectionDetail)
                .joinToString("  ·  ")
            state.isStale -> strings.staleData
            else -> null
        }
        if (warning != null) {
            Text(
                text = warning,
                style = MaterialTheme.typography.bodySmall,
                color = if (connected) StateWaiting else connectionColor(state.connection),
                modifier = Modifier.padding(start = 16.dp, end = 16.dp, bottom = 8.dp),
            )
        }

        HorizontalDivider(color = MaterialTheme.colorScheme.outline)

        if (events.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(
                    text = strings.noEvents,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 12.dp, end = 12.dp, top = 8.dp, bottom = 16.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                // Порядок природний: найдавніше вгорі, найновіше внизу.
                items(events, key = { it.localId }) { event ->
                    EventCard(event, copy)
                }
            }
        }
    }
}

@Composable
private fun DetailHeader(
    task: Protocol.TaskSession,
    nowMs: Long,
    canCopyAll: Boolean,
    onBack: () -> Unit,
    onCopyAll: () -> Unit,
) {
    val strings = LocalStrings.current

    Column(Modifier.padding(horizontal = 16.dp)) {

        Row(
            Modifier.fillMaxWidth().padding(vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = strings.back,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.clickable { onBack() },
            )
            Spacer(Modifier.weight(1f))
            if (canCopyAll) {
                Text(
                    text = strings.copyAll,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .clickable { onCopyAll() }
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                )
                Spacer(Modifier.width(8.dp))
            }
            Text(
                text = task.short,
                style = MaterialTheme.typography.labelSmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Row(verticalAlignment = Alignment.CenterVertically) {
            StatusDot(stateColor(task.state), 12)
            Spacer(Modifier.width(10.dp))
            Text(
                text = stateLabel(task.state, strings),
                style = MaterialTheme.typography.titleMedium,
                color = stateColor(task.state),
            )
            Spacer(Modifier.weight(1f))
            // Таймер поточної відповіді, а не час від створення сесії:
            // сесія може жити днями, і це число нічого не каже.
            TurnTimer(task, nowMs)
        }

        Spacer(Modifier.height(8.dp))
        Text(task.title, style = MaterialTheme.typography.titleLarge)

        // Повний шлях, а не лише назва каталогу: схожі назви проєктів
        // інакше неможливо розрізнити.
        val location = task.cwd ?: task.project
        if (location.isNotEmpty()) {
            Spacer(Modifier.height(4.dp))
            Text(
                text = location,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        task.branch?.let {
            Text(
                text = strings.branch(it),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        // Контекстне вікно — реальна величина з відповіді Claude.
        ContextBar(task)

        // Поточної дії й часу оновлення в шапці немає: те й інше видно
        // в самій стрічці, а тут вони лише забирали місце в розмови.
        Spacer(Modifier.height(10.dp))
    }
}

/**
 * Смуга заповнення контекстного вікна.
 *
 * Показується лише за наявності справжніх даних: якщо межа моделі невідома,
 * виводиться саме число токенів без відсотка — вигадувати частку від
 * невідомої межі не можна (Частина 6 §42 Master Prompt).
 *
 * Кольори смуги від теми не залежать: вони кажуть, скільки лишилося місця.
 */
@Composable
fun ContextBar(task: Protocol.TaskSession) {
    if (task.contextTokens <= 0) return

    val strings = LocalStrings.current
    val percent = task.contextPercent
    val limit = task.contextLimit

    Spacer(Modifier.height(10.dp))

    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = strings.contextLabel,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = buildString {
                append(formatTokens(task.contextTokens))
                if (limit != null) {
                    append(" / ")
                    append(formatTokens(limit))
                    append("  ")
                    append(percent)
                    append('%')
                }
            },
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = contextColor(percent),
        )
    }

    if (percent != null) {
        Spacer(Modifier.height(4.dp))
        LinearProgressIndicator(
            progress = { percent / 100f },
            modifier = Modifier.fillMaxWidth().height(4.dp),
            color = contextColor(percent),
            trackColor = ProgressTrack,
        )
    }
}

/** Заповнення контексту: спокійний колір, доки є запас, тривожний під кінець. */
private fun contextColor(percent: Int?): Color = when {
    percent == null -> StateIdle
    percent >= 90 -> StateError
    percent >= 75 -> StateWaiting
    else -> StateWorking
}

fun formatTokens(tokens: Long): String = when {
    tokens >= 1_000_000 -> "%.1fM".format(java.util.Locale.US, tokens / 1_000_000.0)
    tokens >= 1_000 -> "%.1fk".format(java.util.Locale.US, tokens / 1_000.0)
    else -> tokens.toString()
}

/**
 * Один запис стрічки.
 *
 * Кожен вид події має власний вигляд: репліки Claude виділені підкладкою
 * і читаються як текст, дії — компактним моноширинним рядком, помилки —
 * кольором. Так стрічку можна переглядати очима, не вчитуючись у кожен рядок.
 *
 * Текст кожного запису можна виділити й скопіювати частково, а кнопка
 * праворуч копіює запис цілком.
 */
@Composable
private fun EventCard(event: Protocol.MonitorEvent, onCopy: (String) -> Unit) {
    when (event.kind) {
        Protocol.EventKind.OUTPUT -> OutputBubble(event, onCopy)
        Protocol.EventKind.PROMPT -> PromptBubble(event, onCopy)
        Protocol.EventKind.STATUS, Protocol.EventKind.SESSION -> StatusLine(event)
        else -> CompactLine(event, onCopy)
    }
}

/** Шапка повідомлення: хто й коли написав, праворуч — кнопка копіювання. */
@Composable
private fun BubbleHeader(
    label: String,
    labelColor: Color,
    timestampMs: Long,
    onCopy: () -> Unit,
) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            fontWeight = FontWeight.SemiBold,
            color = labelColor,
        )
        Spacer(Modifier.width(8.dp))
        Text(
            text = formatClock(timestampMs),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.weight(1f))
        CopyButton(onClick = onCopy)
    }
}

/**
 * Повідомлення користувача — праворуч і іншим кольором, як у месенджері.
 * Так розмову видно з першого погляду: де питання, а де відповідь.
 */
@Composable
private fun PromptBubble(event: Protocol.MonitorEvent, onCopy: (String) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
        Column(
            Modifier
                .fillMaxWidth(0.88f)
                .background(
                    MaterialTheme.colorScheme.primary.copy(alpha = 0.16f),
                    RoundedCornerShape(10.dp),
                )
                .padding(start = 12.dp, end = 4.dp, top = 2.dp, bottom = 10.dp)
        ) {
            BubbleHeader(
                label = LocalStrings.current.you,
                labelColor = MaterialTheme.colorScheme.primary,
                timestampMs = event.timestampMs,
                onCopy = { onCopy(event.text.orEmpty()) },
            )
            // Зображень у стрічці немає — лише скільки їх було: самі знімки
            // з ноутбука не виносяться.
            if (event.imageCount > 0) {
                Text(
                    text = LocalStrings.current.imagesAttached(event.imageCount),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.primary,
                    modifier = Modifier.padding(bottom = 2.dp),
                )
            }
            // Без розбору розмітки: у запитах трапляються шляхи та назви
            // на кшталт snake_case, які розбір перетворив би на курсив.
            SelectionContainer(Modifier.padding(end = 8.dp)) {
                Text(
                    text = event.text.orEmpty(),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface,
                )
            }
        }
    }
}

/** Репліка Claude — те, що варто читати, а не проглядати. */
@Composable
private fun OutputBubble(event: Protocol.MonitorEvent, onCopy: (String) -> Unit) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(
                MaterialTheme.colorScheme.surfaceVariant,
                RoundedCornerShape(10.dp),
            )
            .padding(start = 12.dp, end = 4.dp, top = 2.dp, bottom = 10.dp)
    ) {
        BubbleHeader(
            label = LocalStrings.current.claude,
            labelColor = MaterialTheme.colorScheme.onSurfaceVariant,
            timestampMs = event.timestampMs,
            // Копіюється вихідний текст із розміткою — саме так його
            // зручно вставити деінде.
            onCopy = { onCopy(event.text.orEmpty()) },
        )
        // Розмітка розбирається так само, як у самому Claude Code: інакше
        // репліка виглядає як текст, засмічений службовими символами.
        SelectionContainer(Modifier.padding(end = 8.dp)) {
            MarkdownText(
                text = event.text.orEmpty(),
                baseStyle = MaterialTheme.typography.bodyMedium,
                baseColor = MaterialTheme.colorScheme.onSurface,
            )
        }
    }
}

/** Зміна стану — помітний рядок, який ділить стрічку на етапи. */
@Composable
private fun StatusLine(event: Protocol.MonitorEvent) {
    val strings = LocalStrings.current
    val marker = if (event.kind == Protocol.EventKind.SESSION) {
        StatusMarker(statusText(event, strings).uppercase(), null, MarkerKind.STATE)
    } else {
        statusMarker(event.state, event.text, event.turnMs, strings)
    }
    val color = if (event.kind == Protocol.EventKind.SESSION) {
        MaterialTheme.colorScheme.primary
    } else {
        markerColor(marker, event.state)
    }

    Row(
        Modifier.fillMaxWidth().padding(vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = formatClock(event.timestampMs),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.width(8.dp))
        StatusDot(color, 7)
        Spacer(Modifier.width(8.dp))
        val muted = MaterialTheme.colorScheme.onSurfaceVariant
        Text(
            text = buildAnnotatedString {
                withStyle(SpanStyle(fontWeight = FontWeight.SemiBold, color = color)) {
                    append(marker.label)
                }
                // Подробиця — тривалість відповіді чи час скидання ліміту.
                marker.detail?.let { detail ->
                    withStyle(SpanStyle(color = muted)) {
                        append("  ·  ")
                        append(detail)
                    }
                }
            },
            style = MaterialTheme.typography.labelSmall,
        )
    }
}

/** Дія або результат — компактний рядок для швидкого перегляду. */
@Composable
private fun CompactLine(event: Protocol.MonitorEvent, onCopy: (String) -> Unit) {
    val strings = LocalStrings.current
    val isError = event.isError
    val marker = when {
        event.kind == Protocol.EventKind.RESULT && isError -> "✕"
        event.kind == Protocol.EventKind.RESULT -> "✓"
        else -> "›"
    }
    val markerColor = when {
        isError -> StateError
        event.kind == Protocol.EventKind.RESULT -> StateWorking
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }

    Row(Modifier.fillMaxWidth()) {
        Text(
            text = formatClock(event.timestampMs),
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.width(8.dp))

        Text(
            text = marker,
            style = MaterialTheme.typography.labelSmall,
            color = markerColor,
        )
        Spacer(Modifier.width(6.dp))

        SelectionContainer(Modifier.weight(1f)) {
            Column {
                if (event.kind == Protocol.EventKind.ACTIVITY) {
                    Text(
                        text = actionLabel(event.action, strings),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    // Шлях і команда — повністю, без скорочень: скорочений шлях
                    // не впізнати, а обрізана команда не пояснює нічого.
                    event.target?.let {
                        Text(
                            text = it,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = MaterialTheme.colorScheme.onSurface,
                        )
                    }
                } else {
                    // Вивід — теж повністю: найважливіше в ньому часто
                    // саме в тих рядках, які раніше ховались за «…».
                    Text(
                        text = event.text.orEmpty(),
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = if (isError) StateError else MaterialTheme.colorScheme.onSurface,
                    )
                }
            }
        }

        // Кнопка копіює запис цілком одним дотиком — без виділення вручну.
        val fullText = if (event.kind == Protocol.EventKind.ACTIVITY) event.target else event.text
        if (!fullText.isNullOrEmpty()) {
            CopyButton(onClick = { onCopy(fullText) }, size = 28.dp)
        }
    }
}

private fun statusText(event: Protocol.MonitorEvent, strings: Strings): String = when (event.kind) {
    Protocol.EventKind.SESSION -> when (event.text) {
        "started" -> strings.sessionStarted
        "ended" -> strings.sessionEnded
        else -> event.text ?: strings.sessionChanged
    }
    else -> stateLabel(event.state ?: Protocol.ClaudeState.UNKNOWN, strings) +
        (event.text?.let { ": " + strings.bridgeNote(it) } ?: "")
}

/**
 * Уся розмова одним текстом — для кнопки «Копіювати все».
 *
 * Формат простий і читається без застосунку: час, хто говорить, текст.
 * Вставлений у нотатки чи месенджер, він лишається зрозумілим.
 */
private fun chatAsText(
    task: Protocol.TaskSession,
    events: List<Protocol.MonitorEvent>,
    strings: Strings,
): String = buildString {
    append(task.title).append('\n')
    task.cwd?.let { append(it).append('\n') }
    append('\n')

    for (event in events) {
        val time = "[" + formatClock(event.timestampMs) + "] "
        when (event.kind) {
            Protocol.EventKind.PROMPT -> {
                append(time).append(strings.chatYou).append('\n')
                if (event.imageCount > 0) {
                    append(strings.imagesAttached(event.imageCount)).append('\n')
                }
                append(event.text.orEmpty()).append("\n\n")
            }
            Protocol.EventKind.OUTPUT ->
                append(time).append(strings.chatClaude).append('\n')
                    .append(event.text.orEmpty()).append("\n\n")
            Protocol.EventKind.ACTIVITY ->
                append(time).append("› ").append(actionLabel(event.action, strings)).append(' ')
                    .append(event.target.orEmpty()).append('\n')
            Protocol.EventKind.RESULT ->
                append(time).append(if (event.isError) "✕ " else "✓ ")
                    .append(event.text.orEmpty()).append('\n')
            Protocol.EventKind.STATUS -> {
                val marker = statusMarker(event.state, event.text, event.turnMs, strings)
                append(time).append("— ").append(marker.label)
                marker.detail?.let { append(" · ").append(it) }
                append(" —\n")
            }
            Protocol.EventKind.SESSION ->
                append(time).append("— ").append(statusText(event, strings)).append(" —\n")
        }
    }
}.trimEnd()
