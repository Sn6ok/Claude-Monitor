package com.claudemonitor

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.compose.ui.graphics.toArgb
import androidx.core.app.NotificationChannelCompat
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.claudemonitor.data.Protocol
import com.claudemonitor.data.TaskNotification
import com.claudemonitor.i18n.Strings
import com.claudemonitor.ui.folderName
import com.claudemonitor.ui.formatDuration
import com.claudemonitor.ui.formatLimitReset
import com.claudemonitor.ui.theme.StateDone
import com.claudemonitor.ui.theme.StateIdle
import com.claudemonitor.ui.theme.StateLimited
import com.claudemonitor.ui.theme.StateWaiting
import com.claudemonitor.ui.theme.StateWorking

/**
 * Сповіщення застосунку.
 *
 * Два канали: «Задачі Claude» — про зміни стану, і тихий «Фонове з'єднання»
 * для обов'язкового сповіщення фонового сервісу. Користувач може вимкнути
 * будь-який із них у налаштуваннях Android окремо.
 */
class Notifier(private val context: Context) {

    private val manager = NotificationManagerCompat.from(context)

    /** Створює канали або оновлює їхні назви після зміни мови. */
    fun ensureChannels(strings: Strings) {
        val tasks = NotificationChannelCompat
            .Builder(CHANNEL_TASKS, NotificationManagerCompat.IMPORTANCE_HIGH)
            .setName(strings.channelTasks)
            .setDescription(strings.channelTasksDescription)
            .build()
        val background = NotificationChannelCompat
            .Builder(CHANNEL_BACKGROUND, NotificationManagerCompat.IMPORTANCE_MIN)
            .setName(strings.channelBackground)
            .setShowBadge(false)
            .build()
        manager.createNotificationChannelsCompat(listOf(tasks, background))
    }

    /** Постійне сповіщення фонового сервісу — без нього Android сервіс не тримає. */
    fun backgroundNotification(strings: Strings): Notification =
        NotificationCompat.Builder(context, CHANNEL_BACKGROUND)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(strings.backgroundTitle)
            .setContentText(strings.backgroundText)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(openAppIntent(null))
            .build()

    /**
     * Сповіщення про зміну стану задачі.
     *
     * Одна задача — одне сповіщення: нове замінює попереднє, тож «почав»
     * змінюється на «завершив», а не накопичується стосом.
     *
     * @param note подробиця від Bridge: для ліміту — час скидання
     * @param turnMs тривалість завершеної відповіді, якщо відома
     */
    @SuppressLint("MissingPermission") // дозвіл перевіряється нижче
    fun showTask(
        task: Protocol.TaskSession,
        kind: TaskNotification,
        note: String?,
        turnMs: Long,
        strings: Strings,
    ) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        if (!manager.areNotificationsEnabled()) return

        val text = when (kind) {
            TaskNotification.STARTED -> strings.notifStarted
            TaskNotification.FINISHED ->
                strings.notifFinished(if (turnMs > 0) formatDuration(turnMs, strings) else null)
            TaskNotification.INTERRUPTED -> strings.notifInterrupted
            TaskNotification.LIMIT ->
                strings.notifLimit(note?.takeIf { it.isNotBlank() }?.let(::formatLimitReset))
            TaskNotification.WAITING -> strings.notifWaiting
        }
        val color = when (kind) {
            TaskNotification.STARTED -> StateWorking
            TaskNotification.FINISHED -> StateDone
            TaskNotification.INTERRUPTED -> StateIdle
            TaskNotification.LIMIT -> StateLimited
            TaskNotification.WAITING -> StateWaiting
        }

        val notification = NotificationCompat.Builder(context, CHANNEL_TASKS)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(task.title)
            .setContentText(text)
            .setSubText(folderName(task.cwd) ?: task.project.takeIf { it.isNotEmpty() })
            .setColor(color.toArgb())
            .setAutoCancel(true)
            .setCategory(NotificationCompat.CATEGORY_STATUS)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setWhen(System.currentTimeMillis())
            .setShowWhen(true)
            .setContentIntent(openAppIntent(task.sessionId))
            .build()

        manager.notify(TAG_TASK, task.sessionId.hashCode(), notification)
    }

    /** Дотик до сповіщення відкриває застосунок — і саме цю задачу, якщо її вказано. */
    private fun openAppIntent(sessionId: String?): PendingIntent {
        val intent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            if (sessionId != null) putExtra(EXTRA_SESSION, sessionId)
        }
        return PendingIntent.getActivity(
            context,
            sessionId?.hashCode() ?: 0,
            intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
    }

    companion object {
        const val CHANNEL_TASKS = "tasks"
        const val CHANNEL_BACKGROUND = "background"
        const val EXTRA_SESSION = "com.claudemonitor.SESSION"
        private const val TAG_TASK = "task"
    }
}

/** Чи може застосунок зараз показувати сповіщення. */
fun canPostNotifications(context: Context): Boolean {
    if (Build.VERSION.SDK_INT >= 33 &&
        ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
        PackageManager.PERMISSION_GRANTED
    ) {
        return false
    }
    return NotificationManagerCompat.from(context).areNotificationsEnabled()
}
