package com.claudemonitor.ui

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import com.claudemonitor.UiState
import com.claudemonitor.canPostNotifications
import com.claudemonitor.data.AppLanguage
import com.claudemonitor.data.AppSettings
import com.claudemonitor.data.ThemeMode
import com.claudemonitor.i18n.LocalStrings
import com.claudemonitor.ui.theme.StateError
import com.claudemonitor.ui.theme.ThemeSwatches
import com.claudemonitor.ui.theme.colorWithHue
import com.claudemonitor.ui.theme.colorWithLightness
import com.claudemonitor.ui.theme.hslColor
import com.claudemonitor.ui.theme.toHsl

/**
 * Екран підключення телефона до ноутбука.
 *
 * Код одноразовий, живе три хвилини й після використання стає недійсним.
 * Він не є паролем: довготривала довіра тримається на ключах, якими
 * сторони обмінюються під час цієї процедури (Частина 4 §14 Master Prompt).
 */
@Composable
fun PairingScreen(
    state: UiState,
    onRelayUrlChanged: (String) -> Unit,
    onCodeChanged: (String) -> Unit,
    onPair: () -> Unit,
) {
    val strings = LocalStrings.current

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        verticalArrangement = Arrangement.Center,
    ) {
        Text("Claude Monitor", style = MaterialTheme.typography.titleLarge)

        // Телефон потрапив сюди не сам: доступ відкликано. Пояснюємо чому —
        // інакше екран підключення посеред роботи виглядав би як збій.
        if (state.connection == com.claudemonitor.data.RelayClient.ConnectionState.REVOKED) {
            Spacer(Modifier.height(12.dp))
            Text(
                text = strings.revokedNotice,
                style = MaterialTheme.typography.bodyMedium,
                color = StateError,
            )
        }

        Spacer(Modifier.height(8.dp))
        Text(
            text = strings.pairingIntro,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Spacer(Modifier.height(28.dp))

        OutlinedTextField(
            value = state.relayUrlInput,
            onValueChange = onRelayUrlChanged,
            label = { Text(strings.relayAddress) },
            placeholder = { Text(strings.relayPlaceholder) },
            singleLine = true,
            enabled = !state.pairingInProgress,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
            modifier = Modifier.fillMaxWidth(),
        )

        Spacer(Modifier.height(16.dp))

        OutlinedTextField(
            value = state.codeInput,
            onValueChange = onCodeChanged,
            label = { Text(strings.pairingCode) },
            placeholder = { Text("XXXX-XXXX-XXXX") },
            singleLine = true,
            enabled = !state.pairingInProgress,
            keyboardOptions = KeyboardOptions(
                capitalization = KeyboardCapitalization.Characters,
                keyboardType = KeyboardType.Text,
            ),
            textStyle = MaterialTheme.typography.titleMedium.copy(
                fontFamily = FontFamily.Monospace,
                letterSpacing = 2.sp,
            ),
            modifier = Modifier.fillMaxWidth(),
        )

        Spacer(Modifier.height(6.dp))
        Text(
            text = strings.pairingHint,
            style = MaterialTheme.typography.bodySmall,
            fontFamily = FontFamily.Monospace,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        state.pairingError?.let { error ->
            Spacer(Modifier.height(12.dp))
            // Помилка може містити слід виконання — показуємо повністю,
            // інакше причину неможливо встановити без доступу до пристрою.
            Text(
                text = error,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
                color = StateError,
            )
        }

        Spacer(Modifier.height(24.dp))

        Button(
            onClick = onPair,
            enabled = !state.pairingInProgress &&
                state.codeInput.isNotBlank() &&
                state.relayUrlInput.isNotBlank(),
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (state.pairingInProgress) {
                CircularProgressIndicator(
                    modifier = Modifier.height(18.dp).width(18.dp),
                    strokeWidth = 2.dp,
                )
                Spacer(Modifier.width(12.dp))
                Text(strings.connecting)
            } else {
                Text(strings.connect)
            }
        }

        Spacer(Modifier.height(24.dp))
        Text(
            text = strings.pairingFooter,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Start,
        )
    }
}

/**
 * Налаштування: вигляд, мова, сповіщення, сервер і стан безпеки.
 *
 * Стан безпеки показано без прикрас: якщо шифрування наскрізне — так
 * і написано, якщо ні — теж (Частина 6 §42 Master Prompt).
 */
@Composable
fun SettingsScreen(
    state: UiState,
    onBack: () -> Unit,
    onReconnect: () -> Unit,
    onUnpair: () -> Unit,
    onRelayUrlChanged: (String) -> Unit,
    onApplyRelayUrl: () -> Unit,
    onSettingsChange: ((AppSettings) -> AppSettings) -> Unit,
) {
    val strings = LocalStrings.current
    val settings = state.settings

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = strings.back,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
                modifier = Modifier.clickable { onBack() },
            )
        }
        Spacer(Modifier.height(12.dp))
        Text(strings.settingsTitle, style = MaterialTheme.typography.titleLarge)

        // ── Вигляд ───────────────────────────────────────────────────────
        SectionTitle(strings.sectionAppearance)
        Text(
            text = strings.themeLabel,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(6.dp))
        OptionRow(
            options = listOf(
                ThemeMode.DARK to strings.themeDark,
                ThemeMode.LIGHT to strings.themeLight,
                ThemeMode.CUSTOM to strings.themeCustom,
            ),
            selected = settings.themeMode,
            onSelect = { mode -> onSettingsChange { it.copy(themeMode = mode) } },
        )

        if (settings.themeMode == ThemeMode.CUSTOM) {
            Spacer(Modifier.height(12.dp))
            CustomColorPicker(
                color = settings.customColor,
                onColorChange = { color -> onSettingsChange { it.copy(customColor = color) } },
            )
        }

        // ── Мова ─────────────────────────────────────────────────────────
        SectionTitle(strings.sectionLanguage)
        OptionRow(
            options = listOf(
                AppLanguage.UK to "Українська",
                AppLanguage.EN to "English",
            ),
            selected = settings.language,
            onSelect = { language -> onSettingsChange { it.copy(language = language) } },
        )

        // ── Сповіщення ───────────────────────────────────────────────────
        SectionTitle(strings.sectionNotifications)
        NotificationSettings(settings, onSettingsChange)

        // ── Адреса сервера ───────────────────────────────────────────────
        //
        // Змінюється без повторного pairing: безкоштовні тунелі видають нову
        // адресу за кожного запуску, а ключі при цьому лишаються ті самі.
        SectionTitle(strings.sectionServer)

        OutlinedTextField(
            value = state.relayUrlInput,
            onValueChange = onRelayUrlChanged,
            label = { Text(strings.relayLabel) },
            placeholder = { Text("wss://…/ws") },
            singleLine = true,
            textStyle = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
            modifier = Modifier.fillMaxWidth(),
        )

        state.pairingError?.let { error ->
            Spacer(Modifier.height(6.dp))
            Text(error, style = MaterialTheme.typography.bodySmall, color = StateError)
        }

        Spacer(Modifier.height(8.dp))
        OutlinedButton(onClick = onApplyRelayUrl, modifier = Modifier.fillMaxWidth()) {
            Text(strings.saveRelay)
        }

        // ── Стан ─────────────────────────────────────────────────────────
        SectionTitle(strings.sectionStatus)

        SettingRow(strings.rowConnection, connectionLabel(state.connection, strings))
        SettingRow(strings.rowLaptop, if (state.bridgeOnline) strings.online else strings.offline)

        state.bridge?.let { bridge ->
            SettingRow(strings.rowComputer, bridge.host)
            SettingRow(strings.rowBridgeVersion, bridge.version)
            SettingRow(
                strings.rowClaudeDesktop,
                if (bridge.claudeDesktopRunning) strings.running else strings.notRunning,
            )
            SettingRow(strings.rowBridgeMemory, strings.megabytes(bridge.rssMb))
            SettingRow(strings.rowBridgeCpu, "%.1f%%".format(java.util.Locale.US, bridge.cpuPercent))
        }

        // ── Безпека ──────────────────────────────────────────────────────
        SectionTitle(strings.sectionSecurity)

        // Кожне твердження тут відповідає реальній архітектурі.
        SettingRow(strings.rowTransport, "TLS")
        SettingRow(strings.rowEncryption, strings.encryptionValue)
        SettingRow(strings.rowKeys, "Android Keystore")
        SettingRow(strings.rowServerSeesContent, strings.no)
        SettingRow(strings.rowAppMode, strings.readOnly)

        Spacer(Modifier.height(28.dp))

        OutlinedButton(onClick = onReconnect, modifier = Modifier.fillMaxWidth()) {
            Text(strings.reconnect)
        }

        Spacer(Modifier.height(10.dp))

        OutlinedButton(
            onClick = onUnpair,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(strings.unpair, color = StateError)
        }

        Spacer(Modifier.height(8.dp))
        Text(
            text = strings.unpairHint,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        // Версія — щоб було видно, яка збірка стоїть на цьому телефоні:
        // частина можливостей (як-от вузли) з'являється саме з нею.
        val context = LocalContext.current
        val version = remember {
            runCatching {
                context.packageManager.getPackageInfo(context.packageName, 0).versionName
            }.getOrNull().orEmpty()
        }
        if (version.isNotEmpty()) {
            Spacer(Modifier.height(16.dp))
            Text(
                text = "Claude Monitor $version",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/**
 * Сповіщення про задачі.
 *
 * Дозвіл системи запитується лише тоді, коли користувач сам вмикає
 * сповіщення. Якщо його заборонено, перемикач не вдає, що все працює,
 * а показує, де саме дозволити.
 */
@Composable
private fun NotificationSettings(
    settings: AppSettings,
    onSettingsChange: ((AppSettings) -> AppSettings) -> Unit,
) {
    val strings = LocalStrings.current
    val context = LocalContext.current

    var permitted by remember { mutableStateOf(canPostNotifications(context)) }
    var denied by remember { mutableStateOf(false) }

    // Дозвіл могли змінити в налаштуваннях Android, поки екран був згорнутий.
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        permitted = canPostNotifications(context)
        if (permitted) denied = false
    }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) {
        permitted = canPostNotifications(context)
        if (permitted) {
            denied = false
            onSettingsChange { current -> current.copy(notificationsEnabled = true) }
        } else {
            denied = true
        }
    }

    val enabled = settings.notificationsEnabled && permitted

    SwitchRow(
        label = strings.notificationsSwitch,
        hint = strings.notificationsHint,
        checked = enabled,
        onCheckedChange = { on ->
            when {
                !on -> onSettingsChange { it.copy(notificationsEnabled = false) }
                permitted -> onSettingsChange { it.copy(notificationsEnabled = true) }
                Build.VERSION.SDK_INT >= 33 ->
                    permissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                else -> denied = true
            }
        },
    )

    if (!permitted && (denied || settings.notificationsEnabled)) {
        Spacer(Modifier.height(6.dp))
        Text(
            text = strings.notificationsDenied,
            style = MaterialTheme.typography.bodySmall,
            color = StateError,
        )
        Spacer(Modifier.height(6.dp))
        OutlinedButton(
            onClick = {
                runCatching {
                    context.startActivity(
                        Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                            .putExtra(Settings.EXTRA_APP_PACKAGE, context.packageName)
                    )
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(strings.openNotificationSettings)
        }
    }

    if (enabled) {
        Spacer(Modifier.height(4.dp))
        SwitchRow(strings.notifyStartedLabel, settings.notifyStarted, onCheckedChange = { on ->
            onSettingsChange { it.copy(notifyStarted = on) }
        })
        SwitchRow(strings.notifyFinishedLabel, settings.notifyFinished, onCheckedChange = { on ->
            onSettingsChange { it.copy(notifyFinished = on) }
        })
        SwitchRow(strings.notifyLimitLabel, settings.notifyLimit, onCheckedChange = { on ->
            onSettingsChange { it.copy(notifyLimit = on) }
        })
        SwitchRow(strings.notifyWaitingLabel, settings.notifyWaiting, onCheckedChange = { on ->
            onSettingsChange { it.copy(notifyWaiting = on) }
        })

        // Оболонки на кшталт MIUI зупиняють фонові з'єднання попри сервіс —
        // підказуємо, де це вимкнути.
        Spacer(Modifier.height(8.dp))
        Text(
            text = strings.batteryHint,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(6.dp))
        OutlinedButton(
            onClick = {
                runCatching {
                    context.startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
                }.onFailure {
                    runCatching {
                        context.startActivity(
                            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                                .setData(Uri.parse("package:" + context.packageName))
                        )
                    }
                }
            },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(strings.openBatterySettings)
        }
    }
}

/**
 * Вибір кольору власної теми: готові кольори й тонке налаштування.
 *
 * Досить обрати один колір — решта палітри виводиться з нього
 * (див. customColorScheme).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun CustomColorPicker(color: Long, onColorChange: (Long) -> Unit) {
    val strings = LocalStrings.current
    val (hue, saturation, lightness) = Color(color).toHsl()

    Text(
        text = strings.customColorHint,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(10.dp))

    FlowRow(
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        for (swatch in ThemeSwatches) {
            val selected = swatch == color
            Box(
                Modifier
                    .size(38.dp)
                    .clip(CircleShape)
                    .background(Color(swatch))
                    .border(
                        width = if (selected) 3.dp else 1.dp,
                        color = if (selected) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.outline
                        },
                        shape = CircleShape,
                    )
                    .clickable { onColorChange(swatch) }
            )
        }
    }

    Spacer(Modifier.height(14.dp))
    Text(
        text = strings.hueLabel,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(4.dp))
    // Смужка-підказка: який відтінок де на повзунку.
    GradientBar((0..6).map { hslColor(it * 60f, 0.7f, 0.5f) })
    Slider(
        value = hue,
        onValueChange = { onColorChange(colorWithHue(color, it)) },
        valueRange = 0f..360f,
    )

    Text(
        text = strings.lightnessLabel,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(4.dp))
    GradientBar(listOf(hslColor(hue, saturation, 0.05f), hslColor(hue, saturation, 0.5f), hslColor(hue, saturation, 0.97f)))
    Slider(
        value = lightness,
        onValueChange = { onColorChange(colorWithLightness(color, it)) },
        valueRange = 0.05f..0.97f,
    )
}

@Composable
private fun GradientBar(colors: List<Color>) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(8.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(Brush.horizontalGradient(colors))
    )
}

@Composable
private fun SectionTitle(text: String) {
    Spacer(Modifier.height(24.dp))
    Text(text, style = MaterialTheme.typography.titleMedium)
    Spacer(Modifier.height(10.dp))
}

/** Вибір одного варіанта з кількох — у тому ж стилі, що й вкладки головного екрана. */
@Composable
private fun <T> OptionRow(options: List<Pair<T, String>>, selected: T, onSelect: (T) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(10.dp))
            .padding(3.dp),
    ) {
        for ((value, label) in options) {
            val isSelected = value == selected
            Box(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(8.dp))
                    .background(
                        if (isSelected) MaterialTheme.colorScheme.primary.copy(alpha = 0.18f)
                        else Color.Transparent
                    )
                    .selectable(
                        selected = isSelected,
                        role = Role.RadioButton,
                        onClick = { onSelect(value) },
                    )
                    .padding(vertical = 10.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = label,
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = if (isSelected) FontWeight.SemiBold else FontWeight.Normal,
                    color = if (isSelected) {
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

@Composable
private fun SwitchRow(
    label: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    hint: String? = null,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable { onCheckedChange(!checked) }
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            if (hint != null) {
                Spacer(Modifier.height(2.dp))
                Text(
                    text = hint,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.width(12.dp))
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

@Composable
private fun SettingRow(label: String, value: String) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 5.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Text(text = value, style = MaterialTheme.typography.bodySmall)
    }
}
