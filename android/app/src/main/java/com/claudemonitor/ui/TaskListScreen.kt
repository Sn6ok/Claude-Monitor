package com.claudemonitor.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.claudemonitor.UiState
import com.claudemonitor.data.Protocol
import com.claudemonitor.data.RelayClient
import com.claudemonitor.data.TaskFilter
import com.claudemonitor.i18n.LocalStrings
import com.claudemonitor.i18n.Strings
import com.claudemonitor.ui.theme.StateError
import com.claudemonitor.ui.theme.StateWaiting
import com.claudemonitor.ui.theme.StateWorking
import kotlinx.coroutines.delay

/**
 * Головний екран: перелік задач Claude Code, розкладений по вкладках
 * «Всі / Активні / Неактивні».
 *
 * Кожна задача — окрема картка з власним станом, дією та часом роботи.
 * Дані однієї задачі ніколи не потрапляють у картку іншої: усе береться
 * зі стану, ключованого ідентифікатором сесії.
 */
@Composable
fun TaskListScreen(
    state: UiState,
    onOpenTask: (String) -> Unit,
    onFilterChange: (TaskFilter) -> Unit,
    onOpenSettings: () -> Unit,
) {
    // Один спільний таймер на весь екран замість таймера в кожній картці:
    // так час іде рівно, а зайвих перемальовувань немає
    // (Частина 4 §7, §18 Master Prompt).
    var nowMs by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(1000)
            nowMs = System.currentTimeMillis()
        }
    }

    val strings = LocalStrings.current
    val filter = state.taskFilter
    val tasks = state.orderedTasks.filter { filter.matches(it.state) }

    Column(Modifier.fillMaxSize()) {
        HeaderBar(state, onOpenSettings)
        FilterTabs(state, onFilterChange)
        StateChips(state.tasks.values)
        HorizontalDivider(color = MaterialTheme.colorScheme.outline)

        if (tasks.isEmpty() && state.nodes.isEmpty()) {
            EmptyState(state, filter)
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                // Вузли — над задачами: це довідка про ноутбук, а не робота Claude.
                if (state.nodes.isNotEmpty()) {
                    item(key = "nodes") { NodesSection(state.nodes) }
                }

                items(tasks, key = { it.sessionId }) { task ->
                    TaskCard(
                        task = task,
                        nowMs = nowMs,
                        onClick = { onOpenTask(task.sessionId) },
                    )
                }

                if (tasks.isEmpty()) {
                    item(key = "no-tasks") {
                        Text(
                            text = strings.emptyNoTasksTitle,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 8.dp),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun HeaderBar(state: UiState, onOpenSettings: () -> Unit) {
    val strings = LocalStrings.current

    Column(
        Modifier
            .fillMaxWidth()
            .padding(start = 16.dp, end = 6.dp, top = 6.dp, bottom = 12.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = "Claude Monitor",
                    style = MaterialTheme.typography.titleLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    // weight, а не fillMaxWidth: назва поступається місцем
                    // позначці стану, а не витісняє її за край екрана.
                    modifier = Modifier.weight(1f, fill = false),
                )
                Spacer(Modifier.width(8.dp))
                Chip(
                    text = connectionLabel(state.connection, strings),
                    color = connectionColor(state.connection),
                    modifier = Modifier.clickable { onOpenSettings() },
                )
            }
            Spacer(Modifier.width(4.dp))
            // Налаштування — у правому верхньому куті, як звично в застосунках.
            GearButton(onClick = onOpenSettings, description = strings.openSettings)
        }

        Column(Modifier.padding(end = 10.dp)) {
            val detail = state.connectionDetail
            if (detail != null && state.connection != RelayClient.ConnectionState.CONNECTED) {
                Text(
                    text = detail,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 2.dp),
                )
            }

            val bridge = state.bridge
            if (bridge != null) {
                Row(
                    Modifier.padding(top = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = buildString {
                            append(bridge.host)
                            append(" · ")
                            // Показуємо реальні цифри Bridge: користувач має бачити,
                            // що монітор не навантажує ноутбук.
                            append(strings.megabytes(bridge.rssMb))
                            append(" · ")
                            append("%.1f%% CPU".format(java.util.Locale.US, bridge.cpuPercent))
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    if (state.isStale) {
                        // Окрема позначка: дані застаріли — це не те саме,
                        // що спокій (Частина 4 §8 Master Prompt).
                        Spacer(Modifier.width(8.dp))
                        Text(
                            text = strings.staleData,
                            style = MaterialTheme.typography.bodySmall,
                            color = StateWaiting,
                            maxLines = 1,
                        )
                    }
                }
            }
        }
    }
}

/**
 * Вкладки «Всі / Активні / Неактивні».
 *
 * Лічильник видно в кожній вкладці одразу — не треба перемикатися,
 * щоб дізнатися, чи є там щось.
 */
@Composable
private fun FilterTabs(state: UiState, onFilterChange: (TaskFilter) -> Unit) {
    val strings = LocalStrings.current
    val activeCount = state.tasks.values.count { TaskFilter.isActive(it.state) }
    val counts = mapOf(
        TaskFilter.ACTIVE to activeCount,
        TaskFilter.INACTIVE to state.tasks.size - activeCount,
        TaskFilter.ALL to state.tasks.size,
    )

    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 4.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(10.dp))
            .padding(3.dp),
    ) {
        for (tab in listOf(TaskFilter.ALL, TaskFilter.ACTIVE, TaskFilter.INACTIVE)) {
            val selected = state.taskFilter == tab
            Box(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(8.dp))
                    .background(
                        if (selected) MaterialTheme.colorScheme.primary.copy(alpha = 0.18f)
                        else Color.Transparent
                    )
                    .selectable(
                        selected = selected,
                        role = Role.Tab,
                        onClick = { onFilterChange(tab) },
                    )
                    .padding(vertical = 8.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = "${filterLabel(tab, strings)} ${counts[tab] ?: 0}",
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal,
                    color = if (selected) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

private fun filterLabel(filter: TaskFilter, strings: Strings): String = when (filter) {
    TaskFilter.ACTIVE -> strings.filterActive
    TaskFilter.INACTIVE -> strings.filterInactive
    TaskFilter.ALL -> strings.filterAll
}

/**
 * Позначки станів, що потребують погляду: скільки задач працює, чекає
 * на вас або впало з помилкою. Коли таких немає, рядок не займає місця.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun StateChips(tasks: Collection<Protocol.TaskSession>) {
    val strings = LocalStrings.current
    val working = tasks.count { it.state == Protocol.ClaudeState.WORKING }
    val waiting = tasks.count { it.state == Protocol.ClaudeState.WAITING }
    val errors = tasks.count { it.state == Protocol.ClaudeState.ERROR }

    if (working == 0 && waiting == 0 && errors == 0) {
        Spacer(Modifier.height(6.dp))
        return
    }

    // FlowRow, а не Row: за кількох станів одразу чіпи не вміщаються
    // в один рядок на вузькому екрані й раніше виїжджали за край.
    FlowRow(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 8.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        if (working > 0) Chip(strings.chipWorking(working), StateWorking)
        if (waiting > 0) Chip(strings.chipWaiting(waiting), StateWaiting)
        if (errors > 0) Chip(strings.chipErrors(errors), StateError)
    }
}

@Composable
private fun TaskCard(task: Protocol.TaskSession, nowMs: Long, onClick: () -> Unit) {
    val strings = LocalStrings.current
    val color = stateColor(task.state)

    Card(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Column(Modifier.padding(14.dp)) {

            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(color)
                Spacer(Modifier.width(8.dp))
                Text(
                    text = stateLabel(task.state, strings),
                    style = MaterialTheme.typography.labelSmall,
                    color = color,
                    modifier = Modifier.weight(1f),
                )
                Text(
                    text = task.short,
                    style = MaterialTheme.typography.labelSmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Spacer(Modifier.height(6.dp))

            Text(
                text = task.title,
                style = MaterialTheme.typography.titleMedium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )

            // Джерело назви показується явно, коли назва похідна: користувач
            // має розуміти, що це здогадка з тексту запиту, а не задана назва
            // (docs/protocol.md §6.1).
            if (task.titleSource == Protocol.TitleSource.PROMPT ||
                task.titleSource == Protocol.TitleSource.PATH
            ) {
                Text(
                    text = when (task.titleSource) {
                        Protocol.TitleSource.PROMPT -> strings.titleFromPrompt
                        else -> strings.titleFromPath
                    },
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            // У списку — назва папки, у якій живе проєкт. Повний шлях тут
            // лише заважав би: він довгий і в усіх задач починається однаково.
            // Побачити його можна на екрані самої задачі.
            val folder = folderName(task.cwd) ?: task.project
            if (folder.isNotEmpty()) {
                Spacer(Modifier.height(4.dp))
                Text(
                    text = buildString {
                        append(folder)
                        task.branch?.let { append("  ·  $it") }
                    },
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            // Заповнення контексту — щоб було видно, коли розмова
            // наближається до межі моделі.
            ContextBar(task)

            task.activity?.let { activity ->
                Spacer(Modifier.height(8.dp))
                Row {
                    Text(
                        text = "├─ ",
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        // У картці — один рядок незалежно від команди:
                        // картки мають лишатися однакової висоти, щоб список
                        // читався з першого погляду.
                        text = "${actionLabel(activity.action, strings)} " +
                            (activity.target?.replace('\n', ' ') ?: ""),
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurface,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }

            Spacer(Modifier.height(8.dp))

            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    text = "└─ ",
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                // Той самий таймер відповіді, що й на екрані задачі.
                Box(Modifier.weight(1f)) {
                    TurnTimer(task, nowMs, MaterialTheme.typography.bodySmall)
                }
                Text(
                    text = formatAge(task.lastUpdateMs, nowMs, strings),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun EmptyState(state: UiState, filter: TaskFilter) {
    val strings = LocalStrings.current
    val connected = state.connection == RelayClient.ConnectionState.CONNECTED

    // Порожня вкладка при наявних задачах — не те саме, що відсутність
    // задач узагалі: у першому випадку підказуємо, де їх шукати.
    val (title, hint) = when {
        !connected -> strings.emptyNoConnectionTitle to strings.emptyNoConnectionHint
        !state.bridgeOnline -> strings.emptyBridgeOfflineTitle to strings.emptyBridgeOfflineHint
        filter == TaskFilter.ACTIVE && state.tasks.isNotEmpty() ->
            strings.emptyNothingRunningTitle to strings.emptyNothingRunningHint
        filter == TaskFilter.INACTIVE && state.tasks.isNotEmpty() ->
            strings.emptyNoInactiveTitle to strings.emptyNoInactiveHint
        else -> strings.emptyNoTasksTitle to strings.emptyNoTasksHint
    }

    Column(
        Modifier
            .fillMaxSize()
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.titleMedium,
            textAlign = TextAlign.Center,
        )
        Spacer(Modifier.height(8.dp))
        Text(
            text = hint,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
        )
    }
}

/**
 * Картки вузлів.
 *
 * Вузол — розширення, яке власник ноутбука сам поклав у папку `nodes`
 * (docs/nodes.md). Застосунок лише показує те, що вузол надіслав, і чесно
 * каже, коли вузол не дав даних.
 */
@Composable
private fun NodesSection(nodes: List<Protocol.NodeCard>) {
    val strings = LocalStrings.current

    Column(Modifier.fillMaxWidth()) {
        Text(
            text = strings.sectionNodes,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 6.dp),
        )
        for (node in nodes) {
            NodeCardView(node)
            Spacer(Modifier.height(8.dp))
        }
    }
}

@Composable
private fun NodeCardView(node: Protocol.NodeCard) {
    val strings = LocalStrings.current
    val color = when (node.status) {
        "error" -> StateError
        "warn" -> StateWaiting
        else -> StateWorking
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(color, 8)
                Spacer(Modifier.width(8.dp))
                Text(
                    text = node.name,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }

            if (node.error != Protocol.NodeError.NONE) {
                Spacer(Modifier.height(6.dp))
                Text(
                    text = nodeErrorText(node.error, strings),
                    style = MaterialTheme.typography.bodySmall,
                    color = StateError,
                )
                return@Column
            }

            for (line in node.lines) {
                Spacer(Modifier.height(6.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        text = line.label,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.weight(1f),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = line.value,
                        style = MaterialTheme.typography.bodySmall,
                        fontFamily = FontFamily.Monospace,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                }
            }

            node.text?.let { text ->
                Spacer(Modifier.height(8.dp))
                Text(
                    text = text,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurface,
                )
            }
        }
    }
}
