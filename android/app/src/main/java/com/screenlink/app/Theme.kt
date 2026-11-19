package com.screenlink.app

import android.content.Context
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.material3.Typography
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.LayoutDirection

object AppInfo {
    const val NAME = "امتداد"
    const val VERSION = "2.0.1"
    const val DEVELOPER = "عاصم الجهني"
    const val COPYRIGHT = "© 2026 عاصم الجهني. جميع الحقوق محفوظة."
}

/** Light or dark look: 0 follows the device, 1 light, 2 dark (2.0.1). */
object ThemeChoice {
    var mode by mutableIntStateOf(0)
    var systemDark by mutableStateOf(true)
    /** The stream screen is always dark (it sits around the PC picture). */
    var forceDark by mutableStateOf(false)
    var loaded = false
}

/** Colours of the new design (2.0.1): flat cards with thin borders, a navy main button, blue for what is chosen. */
object Palette {
    val dark: Boolean get() = ThemeChoice.forceDark || when (ThemeChoice.mode) { 1 -> false; 2 -> true; else -> ThemeChoice.systemDark }
    private fun pick(light: Long, darkValue: Long) = Color(if (dark) darkValue else light)

    val Background get() = pick(0xFFEEF1F6, 0xFF071230)
    val Panel get() = pick(0xFFFFFFFF, 0xFF0D1B45)
    val PanelBorder get() = pick(0xFFE3E7EF, 0xFF22357A)
    /** Tracks, text fields and quiet fills. */
    val Soft get() = pick(0xFFF1F3F7, 0xFF13245A)
    /** A chosen card. */
    val Selected get() = pick(0xFFF5F8FF, 0xFF12286A)
    /** Fills: switches, chosen icons, sliders. */
    val Accent get() = pick(0xFF1D64D8, 0xFF1D64D8)
    val AccentSoft get() = pick(0xFFE8F0FE, 0xFF16306E)
    /** Main buttons (white text on them). */
    val Ink get() = pick(0xFF0B1530, 0xFF1D64D8)
    /** The big status card. */
    val Hero get() = pick(0xFF0B1530, 0xFF1D64D8)
    val HeroMuted get() = pick(0xFFC3CCE6, 0xFFE3ECFF)
    val Good get() = pick(0xFF15803D, 0xFF4ADE80)
    val GoodSoft get() = pick(0xFFECFDF3, 0xFF0F3A2A)
    val Warn get() = pick(0xFFC2410C, 0xFFFB923C)
    val Danger get() = pick(0xFFB42318, 0xFFF87171)
    val Text get() = pick(0xFF0F1729, 0xFFEAF0FF)
    val TextMuted get() = pick(0xFF5B6478, 0xFFA9B4D6)
    /** Blue text and icons that must stand out. */
    val Glow get() = pick(0xFF1D64D8, 0xFF7CB3FF)
    val Teal = Color(0xFF0E5E63)
    val Purple = Color(0xFF3B2A6E)
    val PanelTop get() = Panel
    val PanelBottom get() = Panel
}

/** IBM Plex Sans Arabic (SIL OFL), as in the design style guide. */
val Plex = FontFamily(
    Font(R.font.plex_regular, FontWeight.Normal),
    Font(R.font.plex_medium, FontWeight.Medium),
    Font(R.font.plex_semibold, FontWeight.SemiBold),
    Font(R.font.plex_bold, FontWeight.Bold),
)

private fun TextStyle.plex() = copy(fontFamily = Plex)
private val PlexTypography = Typography().let { t ->
    Typography(
        displayLarge = t.displayLarge.plex(), displayMedium = t.displayMedium.plex(), displaySmall = t.displaySmall.plex(),
        headlineLarge = t.headlineLarge.plex(), headlineMedium = t.headlineMedium.plex(), headlineSmall = t.headlineSmall.plex(),
        titleLarge = t.titleLarge.plex(), titleMedium = t.titleMedium.plex(), titleSmall = t.titleSmall.plex(),
        bodyLarge = t.bodyLarge.plex(), bodyMedium = t.bodyMedium.plex(), bodySmall = t.bodySmall.plex(),
        labelLarge = t.labelLarge.plex(), labelMedium = t.labelMedium.plex(), labelSmall = t.labelSmall.plex(),
    )
}

/** Light or dark theme (chosen in the settings), right-to-left layout (Arabic UI). */
@Composable
fun ScreenLinkTheme(systemBars: Boolean = true, content: @Composable () -> Unit) {
    val context = LocalContext.current
    if (!ThemeChoice.loaded) {
        ThemeChoice.loaded = true
        ThemeChoice.mode = Prefs.load(context).theme
    }
    val systemDark = isSystemInDarkTheme()
    if (ThemeChoice.systemDark != systemDark) ThemeChoice.systemDark = systemDark
    val dark = Palette.dark
    val background = Palette.Background
    if (systemBars) {
        val window = (context as? android.app.Activity)?.window
        SideEffect {
            if (window != null) {
                window.decorView.setBackgroundColor(background.toArgb())
                window.statusBarColor = background.toArgb()      // ignored on Android 15 (edge to edge), used before it
                window.navigationBarColor = background.toArgb()
                androidx.core.view.WindowCompat.getInsetsController(window, window.decorView).apply {
                    isAppearanceLightStatusBars = !dark
                    isAppearanceLightNavigationBars = !dark
                }
            }
        }
    }
    val scheme = if (dark) {
        darkColorScheme(primary = Palette.Accent, background = background, surface = Palette.Panel, onPrimary = Color.White,
            onBackground = Palette.Text, onSurface = Palette.Text, surfaceVariant = Palette.Soft, outline = Palette.PanelBorder)
    } else {
        lightColorScheme(primary = Palette.Accent, background = background, surface = Palette.Panel, onPrimary = Color.White,
            onBackground = Palette.Text, onSurface = Palette.Text, surfaceVariant = Palette.Soft, outline = Palette.PanelBorder)
    }
    MaterialTheme(
        colorScheme = scheme,
        typography = PlexTypography,
    ) {
        CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Rtl, content = content)
    }
}

/** Counts showing time (kept apart from the other choices so saving them never resets it). */
fun addUsage(context: Context, seconds: Long) {
    val p = context.getSharedPreferences("screenlink", Context.MODE_PRIVATE)
    p.edit().putLong("usageSeconds", p.getLong("usageSeconds", 0L) + seconds).putInt("sessions", p.getInt("sessions", 0) + 1).apply()
}

/** User choices, kept between launches. */
data class Prefs(
    val host: String = "127.0.0.1",
    val mode: String = "MIRROR",
    val fps: Int = 30,
    val quality: Int = 1,        // 0 low, 1 medium, 2 high
    val fullResolution: Boolean = true,
    val audio: Boolean = true,
    val autoStart: Boolean = false,   // start showing as soon as the cable is plugged in
    val touchpad: Boolean = false,    // finger moves the pointer like a laptop touchpad
    val night: Boolean = false,       // warm tint while streaming
    val penOnly: Boolean = false,     // ignore fingers, draw with the pen only
    val drawTools: Boolean = false,   // floating Ctrl / Shift / Alt / Space buttons
    val mac: String = "",             // PC network card, for Wake-on-LAN
    val deviceId: String = "",
    val hevc: Boolean = true,         // use H.265 when the PC and this device both can
    val adaptive: Boolean = true,     // adapt quality to the network
    val windowId: Long = 0,           // show one PC window only (0 = whole screen)
    val windowTitle: String = "",
    val pressure: Int = 1,            // pen pressure feel: 0 soft, 1 normal, 2 firm
    val autoTools: Boolean = true,    // show drawing tools when a drawing program is in front on the PC
    val onboarded: Boolean = false,
    val theme: Int = 0,               // 0 follows the device, 1 light, 2 dark
    val usageSeconds: Long = 0,
    val sessions: Int = 0,
) {
    /** screenW/screenH: the tablet's screen in landscape, in pixels (used for the virtual screen size). */
    fun streamConfig(screenW: Int = 0, screenH: Int = 0): StreamConfig {
        val base = intArrayOf(4_000_000, 8_000_000, 14_000_000)[quality.coerceIn(0, 2)]
        var bitrate = if (fps >= 60) base * 3 / 2 else base
        val extend = mode == "EXTEND"
        val pad = mode == "DRAWPAD"
        var w = 0
        var h = 0
        if (extend && screenW > 0 && screenH > 0) {
            w = if (fullResolution) screenW else screenW / 2
            h = if (fullResolution) screenH else screenH / 2
            if (w * h > 1920 * 1080) bitrate = bitrate * 3 / 2  // more pixels than 1080p
        }
        val maxWidth = if (extend) maxOf(w, 1280) else if (fullResolution) 2560 else 1280
        if (fps >= 120) bitrate = base * 2
        return StreamConfig(fps = fps, bitrate = bitrate, maxWidth = maxWidth, extend = extend, width = w, height = h,
            hevc = hevc && StreamConfig.deviceDecodesHevc(), adaptive = adaptive, window = if (extend) 0 else windowId, pad = pad)
    }

    fun save(context: Context) {
        context.getSharedPreferences("screenlink", Context.MODE_PRIVATE).edit()
            .putString("host", host).putString("mode", mode).putInt("fps", fps)
            .putInt("quality", quality).putBoolean("full", fullResolution).putBoolean("audio", audio)
            .putBoolean("autoStart", autoStart).putBoolean("touchpad", touchpad).putBoolean("night", night)
            .putBoolean("penOnly", penOnly).putBoolean("drawTools", drawTools)
            .putBoolean("hevc", hevc).putBoolean("adaptive", adaptive)
            .putLong("windowId", windowId).putString("windowTitle", windowTitle)
            .putInt("pressure", pressure).putBoolean("autoTools", autoTools).putBoolean("onboarded", onboarded).putInt("theme", theme)
            .apply { if (mac.isNotBlank()) putString("mac", mac) }
            .apply { if (deviceId.isNotBlank()) putString("deviceId", deviceId) }.apply()
    }

    companion object {
        fun load(context: Context): Prefs {
            val p = context.getSharedPreferences("screenlink", Context.MODE_PRIVATE)
            return Prefs(
                host = p.getString("host", "127.0.0.1") ?: "127.0.0.1",
                mode = p.getString("mode", "MIRROR") ?: "MIRROR",
                fps = p.getInt("fps", 30),
                quality = p.getInt("quality", 1),
                fullResolution = p.getBoolean("full", true),
                audio = p.getBoolean("audio", true),
                autoStart = p.getBoolean("autoStart", false),
                touchpad = p.getBoolean("touchpad", false),
                night = p.getBoolean("night", false),
                penOnly = p.getBoolean("penOnly", false),
                drawTools = p.getBoolean("drawTools", false),
                mac = p.getString("mac", "") ?: "",
                hevc = p.getBoolean("hevc", true),
                adaptive = p.getBoolean("adaptive", true),
                windowId = p.getLong("windowId", 0L),
                windowTitle = p.getString("windowTitle", "") ?: "",
                pressure = p.getInt("pressure", 1),
                autoTools = p.getBoolean("autoTools", true),
                onboarded = p.getBoolean("onboarded", false),
                theme = p.getInt("theme", 0),
                usageSeconds = p.getLong("usageSeconds", 0L),
                sessions = p.getInt("sessions", 0),
                deviceId = p.getString("deviceId", null) ?: java.util.UUID.randomUUID().toString().also {
                    p.edit().putString("deviceId", it).apply()
                },
            )
        }
    }
}
