package com.claudemonitor

import androidx.compose.ui.graphics.Color
import com.claudemonitor.ui.theme.ThemeSwatches
import com.claudemonitor.ui.theme.colorWithHue
import com.claudemonitor.ui.theme.contrastRatio
import com.claudemonitor.ui.theme.customColorScheme
import com.claudemonitor.ui.theme.hslColor
import com.claudemonitor.ui.theme.relativeLuminance
import com.claudemonitor.ui.theme.toHsl
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ThemeTest {

    @Test
    fun customThemeStaysReadableForAnyColor() {
        // Хоч який колір обере користувач, текст має лишатися читабельним.
        for (hue in 0 until 360 step 15) {
            for (saturation in listOf(0f, 0.5f, 1f)) {
                for (lightness in listOf(0.02f, 0.2f, 0.35f, 0.49f, 0.5f, 0.65f, 0.8f, 0.99f)) {
                    val seed = hslColor(hue.toFloat(), saturation, lightness)
                    val scheme = customColorScheme(seed)
                    val label = "h=$hue s=$saturation l=$lightness"

                    assertTrue(label, contrastRatio(scheme.onBackground, scheme.background) >= 7.0)
                    assertTrue(label, contrastRatio(scheme.onSurface, scheme.surfaceVariant) >= 4.5)
                    assertTrue(label, contrastRatio(scheme.onSurfaceVariant, scheme.surfaceVariant) >= 4.5)
                    assertTrue(label, contrastRatio(scheme.primary, scheme.surfaceVariant) >= 4.5)
                    assertTrue(label, contrastRatio(scheme.onPrimary, scheme.primary) >= 4.5)
                }
            }
        }
    }

    @Test
    fun deepSwatchesGiveDarkThemesAndPastelOnesLight() {
        for (swatch in ThemeSwatches.take(6)) {
            assertTrue(relativeLuminance(customColorScheme(Color(swatch)).background) < 0.18)
        }
        for (swatch in ThemeSwatches.drop(6)) {
            assertTrue(relativeLuminance(customColorScheme(Color(swatch)).background) > 0.18)
        }
    }

    @Test
    fun backgroundKeepsTheChosenHue() {
        val scheme = customColorScheme(Color(0xFF1E3A5F))
        val (hue, _, _) = scheme.background.toHsl()
        assertEquals(213f, hue, 4f)
    }

    @Test
    fun hslRoundTrip() {
        val (h, s, l) = hslColor(210f, 0.5f, 0.3f).toHsl()
        assertEquals(210f, h, 2f)
        assertEquals(0.5f, s, 0.02f)
        assertEquals(0.3f, l, 0.02f)
    }

    @Test
    fun hueChangeGivesGrayVisibleColor() {
        val (_, saturation, _) = Color(colorWithHue(0xFF2B2B2BL, 120f)).toHsl()
        assertTrue(saturation > 0.3f)
    }
}
