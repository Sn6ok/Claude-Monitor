package com.claudemonitor

import android.app.Application

/**
 * Застосунок.
 *
 * Стан і з'єднання живуть тут, а не в Activity: коли ввімкнені сповіщення,
 * вони мають працювати й тоді, коли екрана застосунку немає.
 */
class MonitorApp : Application() {

    lateinit var controller: MonitorController
        private set

    override fun onCreate() {
        super.onCreate()
        controller = MonitorController(this)
    }
}
