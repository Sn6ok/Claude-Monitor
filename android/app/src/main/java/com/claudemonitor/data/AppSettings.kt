package com.claudemonitor.data

import android.content.Context
import android.content.res.Configuration

/** Тема застосунку: чорна, біла або власна — від обраного кольору. */
enum class ThemeMode { DARK, LIGHT, CUSTOM }

/** Мова інтерфейсу. */
enum class AppLanguage { UK, EN }

/**
 * Налаштування, які обирає користувач.
 *
 * Кольори станів і смуг прогресу від теми не залежать: вони несуть зміст,
 * і звичка «зелений — працює» не повинна ламатися через зміну оформлення.
 */
data class AppSettings(
    val themeMode: ThemeMode = ThemeMode.DARK,
    /** Колір власної теми, ARGB. */
    val customColor: Long = DEFAULT_CUSTOM_COLOR,
    val language: AppLanguage = AppLanguage.UK,

    /**
     * Загальний вимикач сповіщень. Поки він увімкнений, застосунок тримає
     * з'єднання у фоні — інакше дізнатися про зміну стану не було б як.
     */
    val notificationsEnabled: Boolean = false,
    val notifyStarted: Boolean = true,
    val notifyFinished: Boolean = true,
    val notifyLimit: Boolean = true,
    val notifyWaiting: Boolean = true,
) {
    companion object {
        /** Темно-синій, як тло значка застосунку. */
        const val DEFAULT_CUSTOM_COLOR = 0xFF1E3A5FL
    }
}

/** Зберігає налаштування на телефоні. Нічого з цього не залишає пристрій. */
class SettingsStore(context: Context) {

    private val appContext = context.applicationContext
    private val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun load(): AppSettings {
        // Доки користувач тему не обрав, вона відповідає системній.
        val systemDark = (appContext.resources.configuration.uiMode and
            Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
        val defaultTheme = if (systemDark) ThemeMode.DARK else ThemeMode.LIGHT

        return AppSettings(
            themeMode = enumValue(prefs.getString(KEY_THEME, null), defaultTheme),
            customColor = prefs.getLong(KEY_CUSTOM_COLOR, AppSettings.DEFAULT_CUSTOM_COLOR),
            language = enumValue(prefs.getString(KEY_LANGUAGE, null), AppLanguage.UK),
            notificationsEnabled = prefs.getBoolean(KEY_NOTIFICATIONS, false),
            notifyStarted = prefs.getBoolean(KEY_NOTIFY_STARTED, true),
            notifyFinished = prefs.getBoolean(KEY_NOTIFY_FINISHED, true),
            notifyLimit = prefs.getBoolean(KEY_NOTIFY_LIMIT, true),
            notifyWaiting = prefs.getBoolean(KEY_NOTIFY_WAITING, true),
        )
    }

    fun save(settings: AppSettings) {
        prefs.edit()
            .putString(KEY_THEME, settings.themeMode.name)
            .putLong(KEY_CUSTOM_COLOR, settings.customColor)
            .putString(KEY_LANGUAGE, settings.language.name)
            .putBoolean(KEY_NOTIFICATIONS, settings.notificationsEnabled)
            .putBoolean(KEY_NOTIFY_STARTED, settings.notifyStarted)
            .putBoolean(KEY_NOTIFY_FINISHED, settings.notifyFinished)
            .putBoolean(KEY_NOTIFY_LIMIT, settings.notifyLimit)
            .putBoolean(KEY_NOTIFY_WAITING, settings.notifyWaiting)
            .apply()
    }

    private inline fun <reified T : Enum<T>> enumValue(name: String?, default: T): T =
        enumValues<T>().firstOrNull { it.name == name } ?: default

    private companion object {
        const val PREFS = "settings"
        const val KEY_THEME = "theme"
        const val KEY_CUSTOM_COLOR = "custom_color"
        const val KEY_LANGUAGE = "language"
        const val KEY_NOTIFICATIONS = "notifications"
        const val KEY_NOTIFY_STARTED = "notify_started"
        const val KEY_NOTIFY_FINISHED = "notify_finished"
        const val KEY_NOTIFY_LIMIT = "notify_limit"
        const val KEY_NOTIFY_WAITING = "notify_waiting"
    }
}
