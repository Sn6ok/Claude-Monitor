package com.claudemonitor.ui.theme

import android.app.Activity
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import com.claudemonitor.data.AppSettings
import com.claudemonitor.data.ThemeMode
import kotlin.math.abs
import kotlin.math.pow

/**
 * Оформлення застосунку.
 *
 * Палітра свідомо стримана: монітор дивляться мигцем, щоб зрозуміти стан,
 * тож колір несе інформацію, а не прикрашає. Кольори станів підібрані так,
 * щоб відрізнятися і за яскравістю — вони лишаються розрізненними при
 * дальтонізмі, а текстова позначка стану дублює колір завжди
 * (Частина 4 §3 Master Prompt).
 */

// Кольори станів — однакові в усіх темах, зокрема у власній: звичка
// «зелений — працює» не повинна ламатися через зміну оформлення.
val StateWorking = Color(0xFF4CAF50)
val StateWaiting = Color(0xFFFFB300)
val StateIdle = Color(0xFF78909C)
val StateError = Color(0xFFE53935)
val StateUnknown = Color(0xFF9E9E9E)
/** Вичерпано ліміт — фіолетовий: це не помилка й не спокій, а вимушена пауза. */
val StateLimited = Color(0xFF7F77DD)
/** Відповідь завершено — синій, щоб не плутати із зеленим «працює». */
val StateDone = Color(0xFF378ADD)

/** Доріжка смуг прогресу — нейтральна й однакова в усіх темах. */
val ProgressTrack = Color(0x33888888)

private val DarkColors = darkColorScheme(
    primary = Color(0xFFD4A27F),
    onPrimary = Color(0xFF1A1A1A),
    surface = Color(0xFF1C1C1E),
    onSurface = Color(0xFFE8E8EA),
    surfaceVariant = Color(0xFF2A2A2D),
    onSurfaceVariant = Color(0xFFB0B0B5),
    background = Color(0xFF121214),
    onBackground = Color(0xFFE8E8EA),
    error = StateError,
    outline = Color(0xFF3A3A3E),
    surfaceContainerLowest = Color(0xFF0E0E10),
    surfaceContainerLow = Color(0xFF1C1C1E),
    surfaceContainer = Color(0xFF222225),
    surfaceContainerHigh = Color(0xFF262629),
    surfaceContainerHighest = Color(0xFF2A2A2D),
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF9A6B45),
    onPrimary = Color.White,
    surface = Color(0xFFFAFAFA),
    onSurface = Color(0xFF1A1A1C),
    surfaceVariant = Color(0xFFEFEFF1),
    onSurfaceVariant = Color(0xFF55555C),
    background = Color(0xFFFFFFFF),
    onBackground = Color(0xFF1A1A1C),
    error = StateError,
    outline = Color(0xFFD8D8DC),
    surfaceContainerLowest = Color(0xFFFFFFFF),
    surfaceContainerLow = Color(0xFFFAFAFA),
    surfaceContainer = Color(0xFFF4F4F6),
    surfaceContainerHigh = Color(0xFFF1F1F3),
    surfaceContainerHighest = Color(0xFFEFEFF1),
)

/** Готові кольори для власної теми: глибокі — для темного вигляду, пастельні — для світлого. */
val ThemeSwatches: List<Long> = listOf(
    0xFF1E3A5FL, 0xFF1F4D3AL, 0xFF4A1F2EL, 0xFF3B2A5CL, 0xFF124A4FL, 0xFF2B2B2BL,
    0xFFDCE8F7L, 0xFFDDF1E4L, 0xFFF8E1D4L, 0xFFE8E0F5L, 0xFFF3EBD6L, 0xFFF6DCE4L,
)

/**
 * Палітра власної теми з одного кольору.
 *
 * Обраний колір стає тлом (у межах, де текст іще легко читати). Темний колір
 * дає темну тему, світлий — світлу. Решта ролей виводиться з того самого
 * відтінку, а текст і акцент підбираються так, щоб контраст із тлом
 * гарантовано був достатнім за будь-якого кольору.
 */
fun customColorScheme(seed: Color): ColorScheme {
    val (h, s, l) = seed.toHsl()
    return if (l < 0.5f) customDark(h, s, l) else customLight(h, s, l)
}

private fun customDark(h: Float, s: Float, l: Float): ColorScheme {
    val bgS = s.coerceAtMost(0.5f)
    val bgL = l.coerceIn(0.05f, 0.20f)

    val background = hslColor(h, bgS, bgL)
    val surface = hslColor(h, bgS, bgL + 0.025f)
    val surfaceVariant = hslColor(h, bgS * 0.9f, bgL + 0.07f)
    val outline = hslColor(h, bgS * 0.7f, bgL + 0.16f)

    val onBackground = readable(h, 0.12f, 0.92f, surfaceVariant, 7.0, lighter = true)
    val onSurfaceVariant = readable(h, 0.12f, 0.72f, surfaceVariant, 4.5, lighter = true)
    val primary = readable(h, s.coerceIn(0.45f, 0.85f), 0.70f, surfaceVariant, 4.5, lighter = true)
    val onPrimary = readable(h, 0.4f, 0.14f, primary, 4.5, lighter = false)

    return darkColorScheme(
        primary = primary,
        onPrimary = onPrimary,
        primaryContainer = hslColor(h, bgS, bgL + 0.12f),
        onPrimaryContainer = onBackground,
        inversePrimary = hslColor(h, s, 0.35f),
        secondary = primary,
        onSecondary = onPrimary,
        secondaryContainer = surfaceVariant,
        onSecondaryContainer = onBackground,
        tertiary = primary,
        onTertiary = onPrimary,
        background = background,
        onBackground = onBackground,
        surface = surface,
        onSurface = onBackground,
        surfaceVariant = surfaceVariant,
        onSurfaceVariant = onSurfaceVariant,
        surfaceTint = primary,
        inverseSurface = onBackground,
        inverseOnSurface = background,
        error = StateError,
        outline = outline,
        outlineVariant = hslColor(h, bgS * 0.7f, bgL + 0.10f),
        surfaceContainerLowest = hslColor(h, bgS, (bgL - 0.02f).coerceAtLeast(0.02f)),
        surfaceContainerLow = surface,
        surfaceContainer = hslColor(h, bgS, bgL + 0.045f),
        surfaceContainerHigh = hslColor(h, bgS, bgL + 0.06f),
        surfaceContainerHighest = surfaceVariant,
    )
}

private fun customLight(h: Float, s: Float, l: Float): ColorScheme {
    val bgS = s.coerceAtMost(0.6f)
    val bgL = l.coerceIn(0.86f, 0.97f)

    val background = hslColor(h, bgS, bgL)
    val surface = hslColor(h, bgS, (bgL + 0.015f).coerceAtMost(0.985f))
    val surfaceVariant = hslColor(h, bgS * 0.8f, bgL - 0.06f)
    val outline = hslColor(h, bgS * 0.5f, bgL - 0.18f)

    val onBackground = readable(h, 0.25f, 0.10f, surfaceVariant, 7.0, lighter = false)
    val onSurfaceVariant = readable(h, 0.15f, 0.32f, surfaceVariant, 4.5, lighter = false)
    val primary = readable(h, s.coerceIn(0.45f, 0.85f), 0.34f, surfaceVariant, 4.5, lighter = false)
    val onPrimary = readable(h, 0.2f, 0.98f, primary, 4.5, lighter = true)

    return lightColorScheme(
        primary = primary,
        onPrimary = onPrimary,
        primaryContainer = hslColor(h, bgS, bgL - 0.10f),
        onPrimaryContainer = onBackground,
        inversePrimary = hslColor(h, s, 0.75f),
        secondary = primary,
        onSecondary = onPrimary,
        secondaryContainer = surfaceVariant,
        onSecondaryContainer = onBackground,
        tertiary = primary,
        onTertiary = onPrimary,
        background = background,
        onBackground = onBackground,
        surface = surface,
        onSurface = onBackground,
        surfaceVariant = surfaceVariant,
        onSurfaceVariant = onSurfaceVariant,
        surfaceTint = primary,
        inverseSurface = onBackground,
        inverseOnSurface = background,
        error = StateError,
        outline = outline,
        outlineVariant = hslColor(h, bgS * 0.6f, bgL - 0.10f),
        surfaceContainerLowest = hslColor(h, bgS, 0.99f),
        surfaceContainerLow = surface,
        surfaceContainer = hslColor(h, bgS, bgL - 0.02f),
        surfaceContainerHigh = hslColor(h, bgS, bgL - 0.04f),
        surfaceContainerHighest = surfaceVariant,
    )
}

/** Колір того самого відтінку, освітлений чи затемнений до потрібного контрасту. */
private fun readable(
    h: Float,
    s: Float,
    l: Float,
    against: Color,
    minRatio: Double,
    lighter: Boolean,
): Color {
    var lightness = l
    while (lightness in 0f..1f) {
        val color = hslColor(h, s, lightness)
        if (contrastRatio(color, against) >= minRatio) return color
        lightness += if (lighter) 0.02f else -0.02f
    }
    return if (lighter) Color.White else Color.Black
}

// ── Колірна математика ───────────────────────────────────────────────────────

/** Відносна яскравість за WCAG. */
fun relativeLuminance(color: Color): Double {
    fun channel(value: Float): Double {
        val v = value.toDouble()
        return if (v <= 0.03928) v / 12.92 else ((v + 0.055) / 1.055).pow(2.4)
    }
    return 0.2126 * channel(color.red) + 0.7152 * channel(color.green) + 0.0722 * channel(color.blue)
}

/** Контраст двох кольорів за WCAG: від 1 до 21. */
fun contrastRatio(a: Color, b: Color): Double {
    val la = relativeLuminance(a)
    val lb = relativeLuminance(b)
    return (maxOf(la, lb) + 0.05) / (minOf(la, lb) + 0.05)
}

/** Колір з відтінку (0–360), насиченості й світлості (0–1). */
fun hslColor(h: Float, s: Float, l: Float): Color {
    val hue = ((h % 360f) + 360f) % 360f
    val saturation = s.coerceIn(0f, 1f)
    val lightness = l.coerceIn(0f, 1f)

    val c = (1f - abs(2f * lightness - 1f)) * saturation
    val x = c * (1f - abs((hue / 60f) % 2f - 1f))
    val m = lightness - c / 2f
    val (r, g, b) = when {
        hue < 60f -> Triple(c, x, 0f)
        hue < 120f -> Triple(x, c, 0f)
        hue < 180f -> Triple(0f, c, x)
        hue < 240f -> Triple(0f, x, c)
        hue < 300f -> Triple(x, 0f, c)
        else -> Triple(c, 0f, x)
    }
    return Color(
        red = (r + m).coerceIn(0f, 1f),
        green = (g + m).coerceIn(0f, 1f),
        blue = (b + m).coerceIn(0f, 1f),
    )
}

/** Відтінок, насиченість і світлість кольору. */
fun Color.toHsl(): FloatArray {
    val max = maxOf(red, green, blue)
    val min = minOf(red, green, blue)
    val lightness = (max + min) / 2f
    if (max - min < 1e-6f) return floatArrayOf(0f, 0f, lightness)

    val d = max - min
    val saturation = if (lightness > 0.5f) d / (2f - max - min) else d / (max + min)
    val hue = when (max) {
        red -> (green - blue) / d + (if (green < blue) 6f else 0f)
        green -> (blue - red) / d + 2f
        else -> (red - green) / d + 4f
    } * 60f
    return floatArrayOf(hue, saturation, lightness)
}

fun Color.toArgbLong(): Long = toArgb().toLong() and 0xFFFFFFFFL

/** Той самий колір з іншим відтінком. Сірий спершу отримує трохи насиченості — інакше відтінку не видно. */
fun colorWithHue(argb: Long, hue: Float): Long {
    val (_, s, l) = Color(argb).toHsl()
    return hslColor(hue, if (s < 0.08f) 0.4f else s, l).toArgbLong()
}

/** Той самий колір з іншою світлістю. */
fun colorWithLightness(argb: Long, lightness: Float): Long {
    val (h, s, _) = Color(argb).toHsl()
    return hslColor(h, s, lightness).toArgbLong()
}

// ── Шрифти ───────────────────────────────────────────────────────────────────

/** Моноширинний стиль для шляхів, команд і виводу — їх треба читати точно. */
val MonoStyle = TextStyle(
    fontFamily = FontFamily.Monospace,
    fontSize = 12.sp,
    lineHeight = 16.sp,
)

private val AppTypography = Typography(
    titleLarge = TextStyle(fontSize = 20.sp, fontWeight = FontWeight.SemiBold, lineHeight = 26.sp),
    titleMedium = TextStyle(fontSize = 16.sp, fontWeight = FontWeight.SemiBold, lineHeight = 22.sp),
    bodyMedium = TextStyle(fontSize = 14.sp, lineHeight = 20.sp),
    bodySmall = TextStyle(fontSize = 12.sp, lineHeight = 16.sp),
    labelSmall = TextStyle(fontSize = 11.sp, fontWeight = FontWeight.Medium, lineHeight = 14.sp),
)

@Composable
fun ClaudeMonitorTheme(
    settings: AppSettings,
    content: @Composable () -> Unit,
) {
    val colors = when (settings.themeMode) {
        ThemeMode.DARK -> DarkColors
        ThemeMode.LIGHT -> LightColors
        ThemeMode.CUSTOM -> remember(settings.customColor) {
            customColorScheme(Color(settings.customColor))
        }
    }

    // Значки статусбару й панелі навігації — темні на світлому тлі й навпаки.
    // Визначаємо за самим тлом: у власній темі воно може бути будь-яким.
    val lightBars = relativeLuminance(colors.background) > 0.18
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as Activity).window
            val insets = WindowCompat.getInsetsController(window, view)
            insets.isAppearanceLightStatusBars = lightBars
            insets.isAppearanceLightNavigationBars = lightBars
        }
    }

    MaterialTheme(colorScheme = colors, typography = AppTypography, content = content)
}
