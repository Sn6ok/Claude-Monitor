package com.claudemonitor

import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat

/**
 * Фоновий сервіс для сповіщень.
 *
 * Сам він нічого не робить — лише тримає процес живим, поки з'єднанням
 * керує [MonitorController]. Працює тільки тоді, коли користувач увімкнув
 * сповіщення: без них тримати з'єднання у фоні означало б марно витрачати
 * батарею (Частина 4 §12 Master Prompt).
 */
class MonitorService : Service() {

    private val controller: MonitorController
        get() = (application as MonitorApp).controller

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Спершу — startForeground: на це система дає лічені секунди.
        val type = if (Build.VERSION.SDK_INT >= 34) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING
        } else {
            0
        }
        ServiceCompat.startForeground(this, NOTIFICATION_ID, controller.backgroundNotification(), type)

        // Система могла перезапустити сервіс, коли сповіщення вже вимкнено.
        if (!controller.backgroundModeWanted()) {
            stopSelf()
            return START_NOT_STICKY
        }

        controller.onBackgroundServiceStarted()
        return START_STICKY
    }

    override fun onDestroy() {
        controller.onBackgroundServiceStopped()
        super.onDestroy()
    }

    companion object {
        private const val TAG = "MonitorService"
        private const val NOTIFICATION_ID = 1

        fun start(context: Context) {
            runCatching {
                ContextCompat.startForegroundService(context, Intent(context, MonitorService::class.java))
            }.onFailure {
                // Запуск із фону Android 12+ забороняє; спробуємо знову,
                // коли застосунок буде на екрані.
                Log.w(TAG, "не вдалося запустити сервіс: ${it.javaClass.simpleName}")
            }
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, MonitorService::class.java))
        }
    }
}
