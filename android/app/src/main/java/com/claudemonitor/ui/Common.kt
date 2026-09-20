package com.claudemonitor.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.claudemonitor.data.NotificationRules
import com.claudemonitor.data.Protocol
import com.claudemonitor.data.RelayClient
import com.claudemonitor.data.TaskFilter
import com.claudemonitor.i18n.Strings
import com.claudemonitor.ui.theme.StateDone
import com.claudemonitor.ui.theme.StateError
import com.claudemonitor.ui.theme.StateIdle
import com.claudemonitor.ui.theme.StateLimited
import com.claudemonitor.ui.theme.StateUnknown
import com.claudemonitor.ui.theme.StateWaiting
import com.claudemonitor.ui.theme.StateWorking

/** Колір стану задачі. Дублюється текстом — колір ніколи не єдине джерело. */
fun stateColor(state: Protocol.ClaudeState): Color = when (state) {
    Protocol.ClaudeState.WORKING -> StateWorking
    Protocol.ClaudeState.WAITING -> StateWaiting
    Protocol.ClaudeState.ERROR -> StateError
    Protocol.ClaudeState.LIMITED -> StateLimited
    Protocol.ClaudeState.IDLE, Protocol.ClaudeState.FINISHED -> StateIdle
    else -> StateUnknown
}

/** Назва стану — саме її бачить користувач. */
fun stateLabel(state: Protocol.ClaudeState, strings: Strings): String = when (state) {
    Protocol.ClaudeState.WORKING -> strings.stateWorking
    Protocol.ClaudeState.WAITING -> strings.stateWaiting
    Protocol.ClaudeState.IDLE -> strings.stateIdle
    Protocol.ClaudeState.ERROR -> strings.stateError
    Protocol.ClaudeState.LIMITED -> strings.stateLimited
    Protocol.ClaudeState.STARTING -> strings.stateStarting
    Protocol.ClaudeState.FINISHED -> strings.stateFinished
    Protocol.ClaudeState.GONE -> strings.stateGone
    // Це не збій відображення, а чесна відповідь: даних немає
    // (Частина 5 §12, Частина 6 §42 Master Prompt).
    Protocol.ClaudeState.UNKNOWN -> strings.stateUnknown
}

/** Опис дії. Невідома дія показується як є, без вигадок. */
fun actionLabel(action: String?, strings: Strings): String = when (action) {
    "read_file" -> strings.actionRead
    "edit_file" -> strings.actionEdit
    "write_file" -> strings.actionWrite
    "run_command" -> strings.actionRun
    "search" -> strings.actionSearch
    "web_fetch" -> strings.actionFetch
    "task" -> strings.actionTask
    "subagent" -> strings.actionSubagent
    "other", null -> strings.actionOther
    else -> action
}

fun connectionLabel(state: RelayClient.ConnectionState, strings: Strings): String = when (state) {
    RelayClient.ConnectionState.CONNECTED -> strings.connConnected
    RelayClient.ConnectionState.CONNECTING -> strings.connConnecting
    RelayClient.ConnectionState.AUTHENTICATING -> strings.connAuthenticating
    RelayClient.ConnectionState.DISCONNECTED -> strings.connDisconnected
    RelayClient.ConnectionState.UNAUTHORIZED -> strings.connUnauthorized
    RelayClient.ConnectionState.REVOKED -> strings.connRevoked
    RelayClient.ConnectionState.PROTOCOL_MISMATCH -> strings.connProtocolMismatch
}

fun connectionColor(state: RelayClient.ConnectionState): Color = when (state) {
    RelayClient.ConnectionState.CONNECTED -> StateWorking
    RelayClient.ConnectionState.CONNECTING,
    RelayClient.ConnectionState.AUTHENTICATING -> StateWaiting
    RelayClient.ConnectionState.REVOKED,
    RelayClient.ConnectionState.UNAUTHORIZED,
    RelayClient.ConnectionState.PROTOCOL_MISMATCH -> StateError
    RelayClient.ConnectionState.DISCONNECTED -> StateUnknown
}

/** Чому вузол не дав даних — нашою мовою, а не чужим повідомленням. */
fun nodeErrorText(error: Protocol.NodeError, strings: Strings): String = when (error) {
    Protocol.NodeError.START_FAILED -> strings.nodeErrorStart
    Protocol.NodeError.TIMEOUT -> strings.nodeErrorTimeout
    Protocol.NodeError.EXIT_CODE -> strings.nodeErrorExit
    Protocol.NodeError.BAD_OUTPUT -> strings.nodeErrorOutput
    Protocol.NodeError.TOO_LARGE -> strings.nodeErrorTooLarge
    Protocol.NodeError.NONE -> ""
}

/** Час доби для стрічки подій. */
fun formatClock(timestampMs: Long): String {
    if (timestampMs <= 0) return "--:--:--"
    val date = java.util.Date(timestampMs)
    return java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.ROOT).format(date)
}

/** Наскільки давно було оновлення — щоб відрізнити спокій від обриву. */
fun formatAge(lastUpdateMs: Long, nowMs: Long, strings: Strings): String {
    if (lastUpdateMs <= 0) return "—"
    val seconds = (nowMs - lastUpdateMs) / 1000
    return when {
        seconds < 5 -> strings.ageJustNow
        seconds < 60 -> strings.ageSeconds(seconds)
        seconds < 3600 -> strings.ageMinutes(seconds / 60)
        else -> strings.ageHours(seconds / 3600)
    }
}

@Composable
fun StatusDot(color: Color, size: Int = 10) {
    Box(
        modifier = Modifier
            .size(size.dp)
            .background(color, CircleShape)
    )
}

/** Компактна позначка з текстом — використовується для станів і лічильників. */
@Composable
fun Chip(text: String, color: Color, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .background(color.copy(alpha = 0.14f), RoundedCornerShape(6.dp))
            .padding(horizontal = 8.dp, vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StatusDot(color, 7)
        Spacer(Modifier.width(6.dp))
        Text(
            text = text,
            style = MaterialTheme.typography.labelSmall,
            color = color,
        )
    }
}

/**
 * Назва каталогу, у якому працює задача.
 *
 * У списку задач це корисніше за назву проєкту: користувач тримає в голові
 * саме папку, а назва проєкту в кількох задачах може збігатися.
 * Повний шлях лишається на екрані самої задачі.
 */
fun folderName(cwd: String?): String? {
    if (cwd.isNullOrBlank()) return null
    val parts = cwd.split('\\', '/').filter { it.isNotBlank() }
    // Корінь диска (`D:\`) власної назви не має — тоді краще показати як є.
    return parts.lastOrNull() ?: cwd
}

/**
 * Копіює текст у буфер обміну й коротко підтверджує це.
 *
 * Підтвердження показуємо завжди: частина оболонок (зокрема MIUI) власного
 * сповіщення про копіювання не показує, і без нього незрозуміло, чи спрацювало.
 */
fun copyToClipboard(context: android.content.Context, text: String, confirmation: String) {
    if (text.isEmpty()) return
    val clipboard = context.getSystemService(android.content.Context.CLIPBOARD_SERVICE)
        as android.content.ClipboardManager
    clipboard.setPrimaryClip(android.content.ClipData.newPlainText("Claude Monitor", text))
    android.widget.Toast.makeText(context, confirmation, android.widget.Toast.LENGTH_SHORT).show()
}

/**
 * Кнопка «копіювати» — два аркуші один на одному.
 *
 * Значок намальовано вручну: бібліотека значків Material додала б залежність
 * заради одного символу (Частина 6 §39 Master Prompt).
 */
@Composable
fun CopyButton(
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    size: Dp = 32.dp,
    color: Color = MaterialTheme.colorScheme.onSurfaceVariant,
) {
    Box(
        modifier = modifier
            .size(size)
            .clip(CircleShape)
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(14.dp)) {
            val stroke = 1.4.dp.toPx()
            val sheetW = this.size.width * 0.68f
            val sheetH = this.size.height * 0.68f
            val w = this.size.width
            val h = this.size.height

            // Передній аркуш — повністю.
            drawRoundRect(
                color = color,
                topLeft = Offset(0f, h - sheetH),
                size = Size(sheetW, sheetH),
                cornerRadius = CornerRadius(2.dp.toPx()),
                style = Stroke(width = stroke),
            )
            // Задній аркуш — лише та частина, що виглядає з-за переднього.
            val left = w - sheetW
            val top = 0f
            drawLine(color, Offset(left, h - sheetH), Offset(left, top), stroke, StrokeCap.Round)
            drawLine(color, Offset(left, top), Offset(w, top), stroke, StrokeCap.Round)
            drawLine(color, Offset(w, top), Offset(w, sheetH), stroke, StrokeCap.Round)
            drawLine(color, Offset(w, sheetH), Offset(sheetW, sheetH), stroke, StrokeCap.Round)
        }
    }
}

/**
 * Кнопка налаштувань — шестерня.
 *
 * Так само намальована вручну, як і кнопка копіювання: заради одного значка
 * бібліотеку не підключаємо.
 */
@Composable
fun GearButton(
    onClick: () -> Unit,
    description: String,
    modifier: Modifier = Modifier,
    color: Color = MaterialTheme.colorScheme.onSurfaceVariant,
) {
    Box(
        modifier = modifier
            .size(40.dp)
            .clip(CircleShape)
            .clickable(onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(22.dp)) {
            val side = this.size.minDimension
            val c = center
            // Зубці — короткі товсті відрізки навколо кільця.
            repeat(8) { index ->
                rotate(degrees = index * 45f, pivot = c) {
                    drawLine(
                        color = color,
                        start = Offset(c.x, c.y - side * 0.30f),
                        end = Offset(c.x, c.y - side * 0.48f),
                        strokeWidth = side * 0.15f,
                        cap = StrokeCap.Butt,
                    )
                }
            }
            drawCircle(color = color, radius = side * 0.29f, center = c, style = Stroke(width = side * 0.13f))
        }
    }
}

/** Тривалість словами — для підсумку відповіді: «12 с», «3 хв 12 с», «1 год 5 хв». */
fun formatDuration(ms: Long, strings: Strings): String {
    // Частки секунди відкидаємо, як і живий таймер: інакше вгорі було б 1:24,
    // а в рядку під відповіддю — 1 хв 25 с.
    val totalSeconds = ms.coerceAtLeast(0) / 1000
    val hours = totalSeconds / 3600
    val minutes = (totalSeconds % 3600) / 60
    val seconds = totalSeconds % 60
    val h = strings.unitHour
    val m = strings.unitMinute
    val s = strings.unitSecond
    return when {
        hours > 0 -> if (minutes > 0) "$hours $h $minutes $m" else "$hours $h"
        minutes > 0 -> if (seconds > 0) "$minutes $m $seconds $s" else "$minutes $m"
        else -> "$seconds $s"
    }
}

/** Тривалість циферблатом — для живого таймера: «0:07», «3:12», «1:05:07». */
fun formatTimer(ms: Long): String {
    // Годинники ноутбука й телефона можуть трохи розходитися — від'ємного
    // часу на екрані бути не повинно.
    val totalSeconds = ms.coerceAtLeast(0) / 1000
    val hours = totalSeconds / 3600
    val minutes = (totalSeconds % 3600) / 60
    val seconds = totalSeconds % 60
    return if (hours > 0) {
        "%d:%02d:%02d".format(hours, minutes, seconds)
    } else {
        "%d:%02d".format(minutes, seconds)
    }
}

/**
 * Таймер відповіді Claude.
 *
 * Поки Claude працює — живий відлік від вашого запиту. Коли відповідь
 * завершено — скільки вона тривала. Якщо Bridge не знає ні того, ні іншого
 * (наприклад, запустився посеред відповіді), нічого не показуємо:
 * вигаданий час гірший за відсутній.
 */
@Composable
fun TurnTimer(
    task: Protocol.TaskSession,
    nowMs: Long,
    style: TextStyle = MaterialTheme.typography.bodyMedium,
) {
    val running = task.turnStartedAtMs > 0 && TaskFilter.isActive(task.state)
    when {
        running -> Text(
            text = "⏱ " + formatTimer(nowMs - task.turnStartedAtMs),
            style = style,
            fontFamily = FontFamily.Monospace,
            color = stateColor(task.state),
        )
        task.lastTurnMs > 0 -> Text(
            text = "✓ " + formatTimer(task.lastTurnMs),
            style = style,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * Позначка в стрічці: коротка назва й подробиця.
 *
 * Для кінця відповіді назва — «ГОТОВО» чи «ПЕРЕРВАНО», а не загальне
 * «ОЧІКУЄ»: у стрічці важливо, чим закінчилась відповідь, а подробицею
 * йде її тривалість. Для ліміту — до коли він діє.
 */
data class StatusMarker(val label: String, val detail: String?, val kind: MarkerKind)

enum class MarkerKind { DONE, INTERRUPTED, LIMIT, STATE }

private const val DETAIL_SEPARATOR = "  ·  "

fun statusMarker(
    state: Protocol.ClaudeState?,
    text: String?,
    turnMs: Long,
    strings: Strings,
): StatusMarker {
    val duration = if (turnMs > 0) formatDuration(turnMs, strings) else null
    val note = text?.takeIf { it.isNotBlank() }

    return when {
        state == Protocol.ClaudeState.LIMITED -> StatusMarker(
            label = strings.markerLimit,
            detail = listOfNotNull(note?.let { strings.limitUntil(formatLimitReset(it)) }, duration)
                .joinToString(DETAIL_SEPARATOR)
                .ifEmpty { null },
            kind = MarkerKind.LIMIT,
        )
        state == Protocol.ClaudeState.IDLE && note == NotificationRules.INTERRUPTED_NOTE ->
            StatusMarker(strings.markerInterrupted, duration, MarkerKind.INTERRUPTED)
        state == Protocol.ClaudeState.IDLE && duration != null ->
            StatusMarker(strings.markerDone, duration, MarkerKind.DONE)
        else -> StatusMarker(
            label = stateLabel(state ?: Protocol.ClaudeState.UNKNOWN, strings),
            detail = listOfNotNull(note?.let(strings.bridgeNote), duration)
                .joinToString(DETAIL_SEPARATOR)
                .ifEmpty { null },
            kind = MarkerKind.STATE,
        )
    }
}

/** Колір позначки: «готово», «перервано» й «ліміт» мають власні кольори. */
fun markerColor(marker: StatusMarker, state: Protocol.ClaudeState?): Color = when (marker.kind) {
    MarkerKind.DONE -> StateDone
    MarkerKind.INTERRUPTED -> StateIdle
    MarkerKind.LIMIT -> StateLimited
    MarkerKind.STATE -> stateColor(state ?: Protocol.ClaudeState.UNKNOWN)
}

private val RESET_TIME = Regex(
    """(?:([A-Za-z]{3})[a-z]* (\d{1,2}), )?(\d{1,2})(?::(\d{2}))?\s*([ap]m)""",
    RegexOption.IGNORE_CASE,
)
private val MONTHS = listOf("jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec")

/**
 * Час скидання ліміту у звичному вигляді: «10:30pm (Europe/Kyiv)» → «22:30»,
 * «Aug 23, 5pm (Europe/Kyiv)» → «23.08, 17:00». Незнайомий формат
 * показується як є — лише без часового поясу в дужках.
 */
fun formatLimitReset(raw: String): String {
    val match = RESET_TIME.find(raw) ?: return raw.substringBefore(" (").trim()
    val (month, day, hourText, minuteText, meridiem) = match.destructured
    val hour = hourText.toInt() % 12 + if (meridiem.equals("pm", ignoreCase = true)) 12 else 0
    val time = "%02d:%s".format(hour, minuteText.ifEmpty { "00" })
    if (month.isEmpty()) return time
    val monthNumber = MONTHS.indexOf(month.lowercase()) + 1
    return if (monthNumber > 0) "%02d.%02d, %s".format(day.toInt(), monthNumber, time) else time
}
