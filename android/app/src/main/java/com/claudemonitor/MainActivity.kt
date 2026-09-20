package com.claudemonitor

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.claudemonitor.i18n.LocalStrings
import com.claudemonitor.i18n.stringsFor
import com.claudemonitor.ui.PairingScreen
import com.claudemonitor.ui.SettingsScreen
import com.claudemonitor.ui.TaskDetailScreen
import com.claudemonitor.ui.TaskListScreen
import com.claudemonitor.ui.theme.ClaudeMonitorTheme

class MainActivity : ComponentActivity() {

    private val controller: MonitorController
        get() = (application as MonitorApp).controller

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // Запуск дотиком до сповіщення — одразу відкриваємо його задачу.
        if (savedInstanceState == null) handleIntent(intent)

        setContent {
            val state by controller.state.collectAsStateWithLifecycle()

            // Мова й тема беруться з налаштувань і змінюються миттєво,
            // без перезапуску екрана.
            CompositionLocalProvider(LocalStrings provides stringsFor(state.settings.language)) {
                ClaudeMonitorTheme(state.settings) {
                    Surface(
                        modifier = Modifier.fillMaxSize(),
                        color = MaterialTheme.colorScheme.background,
                    ) {
                        // Відступи під системні панелі.
                        //
                        // enableEdgeToEdge розтягує вікно під статусбар і панель
                        // навігації. Без цього відступу заголовок наповзав на
                        // годинник та індикатор батареї, а нижній вміст ховався
                        // за кнопками навігації.
                        Box(
                            Modifier
                                .fillMaxSize()
                                .windowInsetsPadding(WindowInsets.safeDrawing)
                        ) {
                            AppRoot(controller, state)
                        }
                    }
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        val sessionId = intent?.getStringExtra(Notifier.EXTRA_SESSION) ?: return
        // Прибираємо, щоб поворот екрана не відкривав задачу повторно.
        intent.removeExtra(Notifier.EXTRA_SESSION)
        controller.openTaskFromNotification(sessionId)
    }

    /**
     * Видимість застосунку.
     *
     * onStart/onStop, а не onResume/onPause: системний запит дозволу на
     * сповіщення ставить екран на паузу, і з'єднання не повинно рватися
     * через кожне таке вікно.
     */
    override fun onStart() {
        super.onStart()
        controller.onUiVisible()
    }

    override fun onStop() {
        super.onStop()
        controller.onUiHidden()
    }
}

@Composable
private fun AppRoot(controller: MonitorController, state: UiState) {
    // Апаратна кнопка «назад» повертає до списку задач, а не закриває
    // застосунок посеред перегляду.
    BackHandler(enabled = state.screen != AppScreen.TASK_LIST &&
                          state.screen != AppScreen.PAIRING) {
        controller.showTaskList()
    }

    when (state.screen) {
        AppScreen.PAIRING -> PairingScreen(
            state = state,
            onRelayUrlChanged = controller::onRelayUrlChanged,
            onCodeChanged = controller::onCodeChanged,
            onPair = controller::pair,
        )

        AppScreen.TASK_LIST -> TaskListScreen(
            state = state,
            onOpenTask = controller::openTask,
            onFilterChange = controller::setTaskFilter,
            onOpenSettings = controller::showSettings,
        )

        AppScreen.TASK_DETAIL -> TaskDetailScreen(
            state = state,
            onBack = controller::showTaskList,
        )

        AppScreen.SETTINGS -> SettingsScreen(
            state = state,
            onBack = controller::showTaskList,
            onReconnect = controller::reconnect,
            onUnpair = controller::unpair,
            onRelayUrlChanged = controller::onRelayUrlChanged,
            onApplyRelayUrl = controller::applyRelayUrl,
            onSettingsChange = controller::updateSettings,
        )
    }
}
