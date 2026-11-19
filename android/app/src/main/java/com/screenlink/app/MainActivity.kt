@file:OptIn(ExperimentalMaterial3Api::class)

package com.screenlink.app

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDirection
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Link.init(this)
        handle(intent)
        setContent { ScreenLinkTheme { HomeRoot() } }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handle(intent)
    }

    private fun handle(intent: Intent?) {
        if (intent?.getBooleanExtra(EXTRA_START, false) == true) Link.startWhenReady = true
    }

    companion object {
        const val EXTRA_START = "start_stream"
    }
}

private enum class Page { Home, Control, Settings, About }

/** Everything the screens need to show and do. */
private class UiState(
    val prefs: Prefs,
    val connected: Boolean,
    val status: String,
    val hostName: String,
    val hostOs: String,
    val latencyMs: Long?,
    val update: (Prefs) -> Unit,
    val onMode: (String) -> Unit,
    val onConnect: () -> Unit,
    val onDisconnect: () -> Unit,
    val onStart: () -> Unit,
)

@Composable
fun HomeRoot() {
    val context = LocalContext.current
    var prefs by remember { mutableStateOf(Prefs.load(context)) }
    fun update(p: Prefs) { prefs = p; p.save(context) }

    var page by remember { mutableStateOf(Page.Home) }
    LaunchedEffect(prefs.host) { Link.connect(prefs.host) }
    val connected = Link.connected
    val startStream = { context.startActivity(Intent(context, StreamActivity::class.java)) }
    // Start by itself: from the quick-settings tile, or when the cable is plugged in (if chosen).
    LaunchedEffect(connected, Link.startWhenReady) {
        if (!connected || Link.streaming) return@LaunchedEffect
        if (Link.startWhenReady || (prefs.autoStart && prefs.host == "127.0.0.1")) {
            Link.startWhenReady = false
            delay(400)
            startStream()
        }
    }

    val ui = UiState(
        prefs = prefs, connected = connected, status = Link.status, hostName = Link.hostName, hostOs = Link.hostOs,
        latencyMs = Link.latencyMs, update = { update(it) },
        onMode = { update(prefs.copy(mode = it)); if (connected) Link.send(Messages.setMode(it)) },
        onConnect = { Link.connect(prefs.host) },
        onDisconnect = { Link.disconnect() },
        onStart = startStream,
    )

    if (Link.needPin && Link.status == TcpClient.STATUS_CONNECTED) PinDialog()
    if (!prefs.onboarded) Onboarding { update(prefs.copy(onboarded = true)) }

    BoxWithConstraints(Modifier.fillMaxSize().background(Palette.Background)) {
        if (maxWidth >= 900.dp && maxHeight >= 560.dp) {
            TabletLayout(ui, page) { page = it }
        } else {
            PhoneLayout(ui, page) { page = it }
        }
    }
}

// ───────────────────────────── Shared look ─────────────────────────────

@Composable
private fun Glass(
    modifier: Modifier = Modifier,
    radius: Dp = 20.dp,
    padding: Dp = 16.dp,
    spacing: Dp = 10.dp,
    content: @Composable ColumnScope.() -> Unit,
) {
    val shape = RoundedCornerShape(radius)
    Column(
        modifier.clip(shape)
            .background(Palette.Panel)
            .border(1.dp, Palette.PanelBorder, shape)
            .padding(padding),
        verticalArrangement = Arrangement.spacedBy(spacing),
        content = content,
    )
}

@Composable
private fun T(
    text: String, size: TextUnit = 15.sp, weight: FontWeight = FontWeight.Normal,
    color: Color = Palette.Text, modifier: Modifier = Modifier, align: TextAlign? = null, maxLines: Int = Int.MAX_VALUE,
) = Text(text, fontSize = size, fontWeight = weight, color = color, modifier = modifier, textAlign = align, maxLines = maxLines)

@Composable
private fun Ic(id: Int, size: Dp = 22.dp, tint: Color = Palette.Text, modifier: Modifier = Modifier) =
    Icon(painterResource(id), contentDescription = null, tint = tint, modifier = modifier.size(size))

@Composable
private fun Dot(color: Color, size: Dp = 9.dp) =
    Box(Modifier.size(size).shadow(4.dp, CircleShape, ambientColor = color, spotColor = color).clip(CircleShape).background(color))

/** Main buttons and chosen items: one flat navy (light) or blue (dark) colour. */
private val AccentBrush: Brush get() = SolidColor(Palette.Ink)

/** The big glowing blue button. */
@Composable
private fun PrimaryButton(
    text: String, icon: Int?, enabled: Boolean, height: Dp, fontSize: TextUnit, modifier: Modifier = Modifier, onClick: () -> Unit,
) {
    val shape = RoundedCornerShape(height / 2)
    Row(
        modifier.height(height)
            .graphicsLayer { alpha = if (enabled) 1f else 0.45f }
            .clip(shape).background(AccentBrush)
            .clickable(enabled = enabled, onClick = onClick),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        T(text, fontSize, FontWeight.Bold, Color.White)
        if (icon != null) { Spacer(Modifier.width(12.dp)); Ic(icon, (fontSize.value * 1.1f).dp, Color.White) }
    }
}

@Composable
private fun OutlineButton(text: String, icon: Int?, color: Color, height: Dp, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Row(
        modifier.height(height).clip(shape).background(Palette.Panel).border(1.dp, if (color == Palette.Danger) Palette.Danger.copy(alpha = 0.5f) else Palette.PanelBorder, shape)
            .clickable(onClick = onClick).padding(horizontal = 16.dp),
        horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
    ) {
        T(text, 15.sp, FontWeight.SemiBold, if (color == Palette.Danger) Palette.Danger else Palette.Text)
        if (icon != null) { Spacer(Modifier.width(8.dp)); Ic(icon, 20.dp, if (color == Palette.Danger) Palette.Danger else Palette.Text) }
    }
}

@Composable
private fun ConnectedPill() {
    Row(
        Modifier.clip(RoundedCornerShape(50)).background(Palette.GoodSoft).border(1.dp, Palette.Good.copy(alpha = 0.35f), RoundedCornerShape(50))
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Dot(Palette.Good, 6.dp)
        Spacer(Modifier.width(6.dp))
        T("متصل", 13.sp, FontWeight.SemiBold, Palette.Good)
    }
}

/** Joined buttons, one chosen: «أصلية | نصف». */
@Composable
private fun Segmented(options: List<String>, selected: Int, height: Dp = 40.dp, onSelect: (Int) -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Row(
        Modifier.fillMaxWidth().height(height).clip(shape).background(Palette.Soft).padding(3.dp),
    ) {
        options.forEachIndexed { i, label ->
            val on = i == selected
            Box(
                Modifier.weight(1f).fillMaxHeight().clip(RoundedCornerShape(10.dp))
                    .then(if (on) Modifier.background(Palette.Panel).border(1.dp, Palette.PanelBorder, RoundedCornerShape(10.dp)) else Modifier)
                    .clickable { onSelect(i) },
                contentAlignment = Alignment.Center,
            ) { T(label, 14.sp, if (on) FontWeight.Bold else FontWeight.Medium, if (on) Palette.Text else Palette.TextMuted) }
        }
    }
}

@Composable
private fun AudioSwitch(checked: Boolean, onChange: (Boolean) -> Unit) = Switch(
    checked = checked, onCheckedChange = onChange,
    colors = SwitchDefaults.colors(
        checkedThumbColor = Color.White, checkedTrackColor = Palette.Accent, checkedBorderColor = Palette.Accent,
        uncheckedThumbColor = Palette.TextMuted, uncheckedTrackColor = Palette.Soft, uncheckedBorderColor = Palette.PanelBorder,
    ),
)

private data class Mode(val key: String, val title: String, val subtitle: String, val short: String, val icon: Int, val top: Color, val bottom: Color)

private val Modes = listOf(
    Mode("MIRROR", "انعكاس الشاشة", "عرض نفس شاشة الكمبيوتر على جهازك", "عرض نفس شاشة الكمبيوتر", R.drawable.ic_mirror,
        Color(0xFF1D58C9), Color(0xFF0D2C80)),
    Mode("EXTEND", "توسيع الشاشة", "أضف شاشة إضافية لزيادة مساحة العمل", "أضف شاشة إضافية", R.drawable.ic_extend,
        Color(0xFF13656E), Color(0xFF0A3846)),
    Mode("REMOTE", "التحكم عن بُعد", "تحكم الكامل في الكمبيوتر من جهازك", "تحكم في الكمبيوتر عن بُعد", R.drawable.ic_remote,
        Color(0xFF45338C), Color(0xFF241A56)),
    Mode("DRAWPAD", "لوح الرسم", "القلم يرسم على شاشة الكمبيوتر مباشرة", "قلم على شاشة الكمبيوتر", R.drawable.ic_pad,
        Color(0xFF8A5A1E), Color(0xFF4A2E0E)),
)

private fun modeShape(radius: Dp) = RoundedCornerShape(radius)

private fun Modifier.modeSurface(m: Mode, selected: Boolean, radius: Dp): Modifier {
    val shape = modeShape(radius)
    return this
        .clip(shape)
        .background(if (selected) Palette.Selected else Palette.Panel)
        .border(if (selected) 2.dp else 1.dp, if (selected) Palette.Accent else Palette.PanelBorder, shape)
}

private fun Color.brighter() = Color(minOf(1f, red * 1.5f + 0.08f), minOf(1f, green * 1.5f + 0.08f), minOf(1f, blue * 1.5f + 0.08f), alpha)

@Composable
private fun CheckBadge(size: Dp = 26.dp) {
    Box(Modifier.size(size).clip(CircleShape).background(Palette.Accent).border(2.dp, Palette.Panel, CircleShape),
        contentAlignment = Alignment.Center) { Ic(R.drawable.ic_check, size * 0.62f, Color.White) }
}

/** The mode's icon on a rounded square: blue when chosen, quiet grey otherwise. */
@Composable
private fun ModeIcon(m: Mode, selected: Boolean, size: Dp) {
    Box(Modifier.size(size).clip(RoundedCornerShape(size * 0.28f)).background(if (selected) Palette.Accent else Palette.Soft),
        contentAlignment = Alignment.Center) {
        Ic(m.icon, size * 0.55f, if (selected) Color.White else Palette.Text)
    }
}

private fun viaText(ui: UiState) = if (ui.prefs.host == "127.0.0.1") "USB" else "Wi-Fi"

// ───────────────────────────── New design (2.0.1): header, navy main card, modes, floating bar ─────────────────────────────

@Composable
private fun CircleIconButton(icon: Int, label: String, onClick: () -> Unit) {
    Box(
        Modifier.size(48.dp).clip(CircleShape).background(Palette.Panel).border(1.dp, Palette.PanelBorder, CircleShape)
            .clickable(onClickLabel = label, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Ic(icon, 20.dp, Palette.Text) }
}

/** Logo, PC name and two round buttons, at the top of every page. */
@Composable
private fun AppHeader(ui: UiState, compact: Boolean, onPage: (Page) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Image(painterResource(R.drawable.app_logo), contentDescription = null,
            modifier = Modifier.size(if (compact) 44.dp else 48.dp).clip(RoundedCornerShape(14.dp)))
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            if (compact) {
                T(AppInfo.NAME, 13.sp, color = Palette.TextMuted)
                T(if (ui.connected) ui.hostName else "غير متصل", 18.sp, FontWeight.SemiBold, maxLines = 1)
            } else {
                T(if (ui.connected) "${AppInfo.NAME} · ${ui.hostName}" else AppInfo.NAME, 14.sp, color = Palette.TextMuted, maxLines = 1)
                T("حوّل جهازك إلى شاشة للكمبيوتر", 22.sp, FontWeight.SemiBold, maxLines = 1)
            }
        }
        CircleIconButton(R.drawable.ic_wifi, "الأجهزة على شبكة Wi-Fi") { onPage(Page.Settings) }
        Spacer(Modifier.width(10.dp))
        CircleIconButton(R.drawable.ic_info, "حول التطبيق") { onPage(Page.About) }
    }
}

/** A pill-shaped button on the navy card: white (main) or outlined. */
@Composable
private fun HeroPill(text: String, icon: Int?, filled: Boolean, modifier: Modifier = Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(50)
    Row(
        modifier.height(50.dp).clip(shape)
            .then(if (filled) Modifier.background(Color.White) else Modifier.border(1.dp, Color(0x66FFFFFF), shape))
            .clickable(onClick = onClick).padding(horizontal = 20.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center,
    ) {
        if (icon != null) { Ic(icon, 18.dp, if (filled) Palette.Hero else Color.White); Spacer(Modifier.width(8.dp)) }
        T(text, 15.sp, if (filled) FontWeight.Bold else FontWeight.Medium, if (filled) Palette.Hero else Color.White, maxLines = 1)
    }
}

/** Small line of the last latency readings. */
@Composable
private fun Sparkline(values: List<Long>, modifier: Modifier) {
    Canvas(modifier) {
        if (values.size < 2) return@Canvas
        val max = values.max().coerceAtLeast(1L).toFloat()
        val min = values.min().toFloat()
        val span = (max - min).coerceAtLeast(1f)
        val step = size.width / (values.size - 1)
        val path = Path()
        values.forEachIndexed { i, v ->
            val x = size.width - i * step   // right to left: newest on the left
            val y = size.height * 0.1f + (1f - (v - min) / span) * size.height * 0.8f
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        drawPath(path, Color.White.copy(alpha = 0.9f), style = Stroke(width = 2.5.dp.toPx()))
    }
}

/** The navy (light look) or blue (dark look) main card: status, chosen mode and the start button. */
@Composable
private fun HeroCard(ui: UiState, compact: Boolean, onPage: (Page) -> Unit) {
    val p = ui.prefs
    val mode = Modes.firstOrNull { it.key == p.mode } ?: Modes[0]
    var showWindows by remember { mutableStateOf(false) }
    val history = remember { mutableStateListOf<Long>() }
    LaunchedEffect(ui.latencyMs) {
        ui.latencyMs?.let { history.add(0, it); if (history.size > 24) history.removeAt(history.size - 1) }
    }
    val quality = listOf("منخفضة", "متوسطة", "عالية")[p.quality.coerceIn(0, 2)]
    val status = when {
        ui.connected -> "متصل بـ ${ui.hostName} عبر ${viaText(ui)}"
        ui.status == TcpClient.STATUS_DISCONNECTED -> "غير متصل"
        else -> ui.status
    }
    val shape = RoundedCornerShape(if (compact) 22.dp else 24.dp)
    Row(Modifier.fillMaxWidth().clip(shape).background(Palette.Hero).padding(if (compact) 18.dp else 26.dp),
        horizontalArrangement = Arrangement.spacedBy(22.dp)) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(8.dp).clip(CircleShape).background(if (ui.connected) Color(0xFF4ADE80) else Color(0xFFF87171)))
                Spacer(Modifier.width(8.dp))
                T(status, 14.sp, color = Palette.HeroMuted, maxLines = 1)
            }
            Spacer(Modifier.height(6.dp))
            T(if (ui.connected) (if (compact) "جاهز للعرض" else mode.title) else "غير متصل", if (compact) 26.sp else 34.sp,
                FontWeight.SemiBold, Color.White, maxLines = 1)
            T(
                if (ui.connected) {
                    (if (compact) "${mode.title} · " else "") + "${p.fps} إطار/ث · جودة $quality" +
                        (if (p.hevc) " · H.265" else " · H.264") + (if (compact) ui.latencyMs?.let { " · ${it}ms" } ?: "" else "")
                } else "شغّل برنامج ${AppInfo.NAME} على الكمبيوتر، ثم صِل الكيبل أو ابحث عنه عبر Wi-Fi.",
                14.sp, color = Palette.HeroMuted, maxLines = 2,
            )
            Spacer(Modifier.height(if (compact) 16.dp else 22.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                val w = if (compact) Modifier.weight(1f) else Modifier
                if (ui.connected) {
                    HeroPill("بدء العرض", R.drawable.ic_play, true, w, ui.onStart)
                    if (compact) {
                        HeroPill("لوحة التحكم", R.drawable.ic_control, false, w) { onPage(Page.Control) }
                    } else {
                        if (p.mode != "EXTEND") HeroPill("اختيار نافذة", null, false) { Link.requestWindows(); showWindows = true }
                        HeroPill("قطع الاتصال", null, false, onClick = ui.onDisconnect)
                    }
                } else {
                    HeroPill("اتصال", R.drawable.ic_link, true, w, ui.onConnect)
                    HeroPill("البحث عبر Wi-Fi", R.drawable.ic_search, false, w) { onPage(Page.Settings) }
                }
            }
        }
        if (!compact) {
            Column(Modifier.width(230.dp).clip(RoundedCornerShape(18.dp)).background(Color(0x1FFFFFFF)).padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(4.dp)) {
                T("زمن الاستجابة", 13.sp, color = Palette.HeroMuted)
                T(ui.latencyMs?.let { "$it ms" } ?: "—", 28.sp, FontWeight.SemiBold, Color.White)
                Sparkline(history.toList(), Modifier.fillMaxWidth().height(56.dp))
                T(if (ui.connected) "عبر ${viaText(ui)}" else "غير متصل", 12.sp, color = Palette.HeroMuted)
            }
        }
    }
    if (showWindows) WindowDialog(ui) { showWindows = false }
}

@Composable
private fun RadioMark(selected: Boolean) {
    Box(Modifier.size(22.dp).clip(CircleShape)
        .border(if (selected) 6.dp else 2.dp, if (selected) Palette.Accent else Palette.TextMuted.copy(alpha = 0.45f), CircleShape))
}

/** A mode as a tall card (tablet): icon, round mark, name and a line about it. */
@Composable
private fun ModeCard(m: Mode, selected: Boolean, modifier: Modifier, onClick: () -> Unit) {
    Column(
        modifier.heightIn(min = 170.dp).fillMaxHeight().modeSurface(m, selected, 20.dp).clickable(onClick = onClick).padding(18.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.Top) {
            ModeIcon(m, selected, 48.dp)
            RadioMark(selected)
        }
        Spacer(Modifier.weight(1f))
        T(m.title, 17.sp, FontWeight.SemiBold, maxLines = 1)
        T(m.subtitle, 13.sp, color = Palette.TextMuted, maxLines = 2)
    }
}

/** A mode as a small tile (phone, two by two). */
@Composable
private fun ModeChip(m: Mode, selected: Boolean, modifier: Modifier, onClick: () -> Unit) {
    Row(
        modifier.height(64.dp).modeSurface(m, selected, 16.dp).clickable(onClick = onClick).padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ModeIcon(m, selected, 38.dp)
        Spacer(Modifier.width(10.dp))
        T(m.title, 14.sp, FontWeight.Medium, maxLines = 1)
    }
}

@Composable
private fun ModesSection(ui: UiState, wide: Boolean) {
    val current = Modes.firstOrNull { it.key == ui.prefs.mode } ?: Modes[0]
    Row(verticalAlignment = Alignment.CenterVertically) {
        T("اختر وضع العرض", 18.sp, FontWeight.SemiBold, modifier = Modifier.weight(1f))
        if (wide) ProfileChips(ui) else T(current.title, 13.sp, color = Palette.TextMuted)
    }
    if (wide) {
        Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            Modes.forEach { m -> ModeCard(m, ui.prefs.mode == m.key, Modifier.weight(1f)) { ui.onMode(m.key) } }
        }
    } else {
        Modes.chunked(2).forEach { row ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                row.forEach { m -> ModeChip(m, ui.prefs.mode == m.key, Modifier.weight(1f)) { ui.onMode(m.key) } }
            }
        }
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
        T(label, 15.sp, FontWeight.Medium, modifier = Modifier.weight(1f))
        AudioSwitch(checked, onChange)
    }
}

/** "جودة البث": frames, resolution, quality and the switches (tablet side column). */
@Composable
private fun QualityCard(ui: UiState) {
    val p = ui.prefs
    Glass(Modifier.fillMaxWidth(), radius = 24.dp, padding = 22.dp, spacing = 10.dp) {
        T("جودة البث", 18.sp, FontWeight.SemiBold)
        T("الإطارات في الثانية", 13.sp, color = Palette.TextMuted)
        Segmented(listOf("30", "60", "120"), when { p.fps >= 120 -> 2; p.fps >= 60 -> 1; else -> 0 }) {
            ui.update(p.copy(fps = when (it) { 2 -> 120; 1 -> 60; else -> 30 }))
        }
        T("الدقة", 13.sp, color = Palette.TextMuted)
        Segmented(listOf("أصلية", "نصف"), if (p.fullResolution) 0 else 1) { ui.update(p.copy(fullResolution = it == 0)) }
        T("الجودة", 13.sp, color = Palette.TextMuted)
        Segmented(listOf("منخفضة", "متوسطة", "عالية"), p.quality.coerceIn(0, 2)) { ui.update(p.copy(quality = it)) }
        HorizontalDivider(color = Palette.PanelBorder, modifier = Modifier.padding(vertical = 4.dp))
        ToggleRow("ترميز H.265 (أوضح)", p.hevc) { ui.update(p.copy(hevc = it)) }
        ToggleRow("جودة تتكيف مع الشبكة", p.adaptive) { ui.update(p.copy(adaptive = it)) }
        ToggleRow("صوت الكمبيوتر", p.audio) { ui.update(p.copy(audio = it)) }
        ToggleRow("ابدأ عند توصيل الكيبل", p.autoStart) { ui.update(p.copy(autoStart = it)) }
        Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Palette.Soft).padding(12.dp)) {
            Ic(R.drawable.ic_info, 18.dp, Palette.TextMuted)
            Spacer(Modifier.width(10.dp))
            T("جيد: الدقة الأصلية و60 إطارًا وجودة متوسطة إلى عالية.", 13.sp)
        }
    }
}

/** The floating pill at the bottom: the chosen page is a navy pill with its name, the others are icons. */
@Composable
private fun PillBar(page: Page, onPage: (Page) -> Unit, fill: Boolean, modifier: Modifier) {
    val shape = RoundedCornerShape(50)
    Row(
        modifier.shadow(10.dp, shape).clip(shape).background(Palette.Panel).border(1.dp, Palette.PanelBorder, shape).padding(6.dp),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        val items = listOf(
            Triple(Page.Home, "الرئيسية", R.drawable.ic_home),
            Triple(Page.Control, "التحكم", R.drawable.ic_control),
            Triple(Page.Settings, "الإعدادات", R.drawable.ic_settings),
            Triple(Page.About, "حول", R.drawable.ic_info),
        )
        items.forEach { (target, label, icon) ->
            val on = page == target
            val size = if (fill) Modifier.weight(if (on) 1.7f else 1f) else if (on) Modifier else Modifier.width(64.dp)
            Row(
                size.height(52.dp).clip(shape).then(if (on) Modifier.background(Palette.Ink) else Modifier)
                    .clickable(onClickLabel = label) { onPage(target) }.padding(horizontal = if (on) 20.dp else 0.dp),
                verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.Center,
            ) {
                Ic(icon, 20.dp, if (on) Color.White else Palette.Text)
                if (on) { Spacer(Modifier.width(8.dp)); T(label, 15.sp, FontWeight.Medium, Color.White, maxLines = 1) }
            }
        }
    }
}

// ───────────────────────────── Tablet ─────────────────────────────

@Composable
private fun TabletLayout(ui: UiState, page: Page, onPage: (Page) -> Unit) {
    Box(Modifier.fillMaxSize().systemBarsPadding()) {
        Column(Modifier.fillMaxSize().padding(start = 28.dp, end = 28.dp, top = 22.dp)) {
            AppHeader(ui, compact = false, onPage)
            Spacer(Modifier.height(20.dp))
            if (page == Page.Home) {
                Row(Modifier.weight(1f).padding(bottom = 92.dp), horizontalArrangement = Arrangement.spacedBy(22.dp)) {
                    Column(Modifier.weight(1f).fillMaxHeight().verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(20.dp)) {
                        HeroCard(ui, compact = false, onPage)
                        ModesSection(ui, wide = true)
                    }
                    Column(Modifier.width(360.dp).fillMaxHeight().verticalScroll(rememberScrollState())) { QualityCard(ui) }
                }
            } else {
                Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = 100.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp)) {
                    when (page) {
                        Page.Control -> ControlPage(ui, wide = true)
                        Page.Settings -> Column(Modifier.widthIn(max = 760.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
                            WifiPage(ui, onBack = { onPage(Page.Home) })
                        }
                        else -> Column(Modifier.widthIn(max = 760.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) { AboutPage() }
                    }
                }
            }
        }
        PillBar(page, onPage, fill = false, modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 20.dp))
    }
}

// ───────────────────────────── Phone ─────────────────────────────

@Composable
private fun PhoneLayout(ui: UiState, page: Page, onPage: (Page) -> Unit) {
    Box(Modifier.fillMaxSize().systemBarsPadding()) {
        Column(
            Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(start = 18.dp, end = 18.dp, top = 14.dp, bottom = 96.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            AppHeader(ui, compact = true, onPage)
            when (page) {
                Page.Home -> PhoneHome(ui, onPage)
                Page.Control -> ControlPage(ui, wide = false)
                Page.Settings -> WifiPage(ui, onBack = { onPage(Page.Home) })
                Page.About -> AboutPage()
            }
        }
        PillBar(page, onPage, fill = true,
            modifier = Modifier.align(Alignment.BottomCenter).fillMaxWidth().padding(start = 18.dp, end = 18.dp, bottom = 14.dp))
    }
}

@Composable
private fun ColumnScope.PhoneHome(ui: UiState, onPage: (Page) -> Unit) {
    HeroCard(ui, compact = true, onPage)
    ModesSection(ui, wide = false)
    WindowChoice(ui)
    val p = ui.prefs
    Glass(Modifier.fillMaxWidth(), padding = 16.dp, spacing = 10.dp) {
        T("خيارات العرض", 16.sp, FontWeight.SemiBold)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            CycleChip(if (p.fullResolution) "أصلية" else "نصف", p.fullResolution, Modifier.weight(1f)) {
                ui.update(p.copy(fullResolution = !p.fullResolution))
            }
            CycleChip("${p.fps} FPS", p.fps >= 60, Modifier.weight(1f)) {
                ui.update(p.copy(fps = when { p.fps >= 120 -> 30; p.fps >= 60 -> 120; else -> 60 }))
            }
            CycleChip(listOf("منخفضة", "متوسطة", "عالية")[p.quality], p.quality == 2, Modifier.weight(1f)) {
                ui.update(p.copy(quality = (p.quality + 1) % 3))
            }
        }
        ToggleRow("صوت الكمبيوتر", p.audio) { ui.update(p.copy(audio = it)) }
        ToggleRow("ابدأ عند توصيل الكيبل", p.autoStart) { ui.update(p.copy(autoStart = it)) }
    }
}

/** One option that changes with each tap; highlighted at its best setting. */
@Composable
private fun CycleChip(label: String, best: Boolean, modifier: Modifier, onClick: () -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Box(
        modifier.height(44.dp).clip(shape)
            .then(if (best) Modifier.background(AccentBrush) else Modifier.background(Palette.Soft))
            .border(1.dp, if (best) Color.Transparent else Palette.PanelBorder, shape).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { T(label, 14.sp, FontWeight.SemiBold, if (best) Color.White else Palette.Text) }
}

// ───────────────────────────── Wi-Fi / settings ─────────────────────────────

@Composable
private fun ColumnScope.WifiPage(ui: UiState, onBack: () -> Unit) {
    val prefs = ui.prefs
    var host by remember(prefs.host) { mutableStateOf(if (prefs.host == "127.0.0.1") "" else prefs.host) }
    var searching by remember { mutableStateOf(false) }
    var found by remember { mutableStateOf<List<FoundHost>>(emptyList()) }
    var searched by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    fun search() {
        searching = true
        scope.launch { found = Discovery.search(); searching = false; searched = true }
    }
    LaunchedEffect(Unit) { search() }

    Row(verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = onBack) { Ic(R.drawable.ic_arrow_right, 26.dp) }
        T("الأجهزة على شبكة Wi-Fi", 22.sp, FontWeight.Bold)
    }
    Column(Modifier.fillMaxWidth().clickable(enabled = !searching) { search() }.padding(vertical = 6.dp),
        horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (searching) {
            CircularProgressIndicator(Modifier.size(30.dp), color = Palette.Glow, strokeWidth = 3.dp, trackColor = Palette.Soft)
            T("جارٍ البحث عن أجهزة الكمبيوتر …", 15.sp, color = Palette.TextMuted)
        } else {
            Ic(R.drawable.ic_search, 28.dp, Palette.Glow)
            T(if (found.isEmpty() && searched) "لم يُعثر على كمبيوتر. اضغط للبحث مرة أخرى" else "اضغط للبحث مرة أخرى", 15.sp,
                color = Palette.TextMuted, align = TextAlign.Center)
        }
    }
    found.forEach { h ->
        Glass(Modifier.fillMaxWidth(), padding = 16.dp) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Ic(R.drawable.ic_pc, 34.dp)
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    T(h.name, 17.sp, FontWeight.Bold)
                    T(h.address, 14.sp, color = Palette.TextMuted)
                }
                val current = prefs.host == h.address && ui.connected
                if (current) ConnectedPill()
                else PrimaryButton("اتصال", null, true, 38.dp, 14.sp, Modifier.width(90.dp)) { ui.update(prefs.copy(host = h.address)) }
            }
        }
    }
    if (searched && found.isEmpty() && !searching) {
        T("تأكد أن برنامج ${AppInfo.NAME} يعمل على الكمبيوتر، وأن الجهازين على نفس الشبكة، وأنك فعّلت «السماح بالاتصال عبر Wi-Fi» من قائمته.",
            13.sp, color = Palette.TextMuted, align = TextAlign.Center, modifier = Modifier.fillMaxWidth())
    }

    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 6.dp)) {
        HorizontalDivider(Modifier.weight(1f), color = Palette.PanelBorder)
        T("  أو الاتصال يدويًا  ", 15.sp, color = Palette.TextMuted)
        HorizontalDivider(Modifier.weight(1f), color = Palette.PanelBorder)
    }
    T("عنوان IP للكمبيوتر", 14.sp, color = Palette.TextMuted)
    val fieldShape = RoundedCornerShape(12.dp)
    BasicTextField(
        value = host, onValueChange = { host = it }, singleLine = true,
        textStyle = TextStyle(color = Palette.Text, fontSize = 18.sp, fontFamily = Plex, textAlign = TextAlign.Center,
            textDirection = TextDirection.Ltr),
        cursorBrush = SolidColor(Palette.Glow),
        modifier = Modifier.fillMaxWidth().height(52.dp).clip(fieldShape).background(Palette.Soft)
            .border(1.dp, Palette.PanelBorder, fieldShape).padding(horizontal = 16.dp, vertical = 14.dp),
        decorationBox = { inner ->
            Box(contentAlignment = Alignment.Center) {
                if (host.isEmpty()) T("192.168.1.20", 18.sp, color = Palette.TextMuted.copy(alpha = 0.6f))
                inner()
            }
        },
    )
    PrimaryButton("حفظ واتصال", R.drawable.ic_link, host.isNotBlank(), 52.dp, 17.sp, Modifier.fillMaxWidth()) {
        ui.update(prefs.copy(host = host.trim()))
    }
    OutlineButton("USB", R.drawable.ic_usb, Palette.PanelBorder, 50.dp, Modifier.fillMaxWidth()) {
        ui.update(prefs.copy(host = "127.0.0.1"))
    }
    T("عبر USB يربط برنامج الكمبيوتر الكيبل تلقائيًا. عبر Wi-Fi يجب أن يكون الجهازان على نفس الشبكة.", 13.sp,
        color = Palette.TextMuted, align = TextAlign.Center, modifier = Modifier.fillMaxWidth())
    Spacer(Modifier.height(8.dp))
    Glass(Modifier.fillMaxWidth(), spacing = 8.dp) {
        T("المظهر", 17.sp, FontWeight.Bold)
        Segmented(listOf("تلقائي", "فاتح", "داكن"), ThemeChoice.mode) {
            ThemeChoice.mode = it
            ui.update(ui.prefs.copy(theme = it))
        }
        T("«تلقائي» يتبع وضع الجهاز الليلي. شاشة العرض تبقى داكنة دائمًا.", 13.sp, color = Palette.TextMuted)
    }
    PenSettings(ui)
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            T("أدوات الرسم تلقائيًا مع برامج الرسم", 15.sp, FontWeight.SemiBold, modifier = Modifier.weight(1f))
            AudioSwitch(ui.prefs.autoTools) { ui.update(ui.prefs.copy(autoTools = it)) }
        }
        T("تظهر أزرار Ctrl وShift وAlt أثناء العرض عندما يكون Photoshop أو Krita أو غيرها في المقدمة على الكمبيوتر.", 13.sp, color = Palette.TextMuted)
    }
    Diagnostics(ui)
}

@Composable
private fun ColumnScope.AboutPage() {
    val context0 = LocalContext.current
    Glass(Modifier.fillMaxWidth(), padding = 20.dp) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Image(painterResource(R.drawable.app_logo), contentDescription = null, modifier = Modifier.size(64.dp).clip(RoundedCornerShape(16.dp)))
            Spacer(Modifier.width(14.dp))
            Column {
                T(AppInfo.NAME, 24.sp, FontWeight.Bold)
                T("الإصدار ${AppInfo.VERSION}", 14.sp, color = Palette.TextMuted)
            }
        }
        T("تطوير: ${AppInfo.DEVELOPER}", 18.sp, FontWeight.Bold)
        T(AppInfo.COPYRIGHT, 14.sp, color = Palette.TextMuted)
        T("يجعل التابلت أو الجوال شاشة ثانية لكمبيوتر Windows أو مرآة له، مع التحكم باللمس والقلم ونقل الصوت.", 15.sp, color = Palette.TextMuted)
        T("البث: H.265 أو H.264 بترميز كرت الشاشة، بجودة تتكيف مع الشبكة. الاتصال: كيبل USB أو Wi-Fi.", 15.sp, color = Palette.TextMuted)
        T("الشاشة الثانية تعتمد على تعريف Virtual Display Driver المفتوح المصدر.", 15.sp, color = Palette.TextMuted)
        val usage = remember { Prefs.load(context0) }
        T("استخدامك: ${usage.sessions} مرة عرض، ${usage.usageSeconds / 3600} ساعة و${usage.usageSeconds % 3600 / 60} دقيقة.", 15.sp, color = Palette.Text)
        T("لوحة التحكم: أزرار الميديا والبرامج والطاقة، ومراقبة الأداء، والحافظة المشتركة، وتشغيل الكمبيوتر عن بُعد.", 15.sp, color = Palette.TextMuted)
        T("الخط: IBM Plex Sans Arabic (رخصة SIL OFL). الأيقونات: Lucide (رخصة ISC).", 13.sp, color = Palette.TextMuted)
    }
}

// ───────────────────────────── PIN (Wi-Fi) ─────────────────────────────

@Composable
private fun PinDialog() {
    var pin by remember { mutableStateOf("") }
    AlertDialog(
        onDismissRequest = { Link.dismissPin() },
        containerColor = Palette.Panel,
        title = { T("رمز الاتصال", 20.sp, FontWeight.Bold) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                T("اكتب الرمز الظاهر في نافذة امتداد على الكمبيوتر (بطاقة Wi-Fi). يُطلب مرة واحدة لكل جهاز.", 14.sp, color = Palette.TextMuted)
                val shape = RoundedCornerShape(12.dp)
                BasicTextField(
                    value = pin, onValueChange = { v -> pin = v.filter { it.isDigit() }.take(6) }, singleLine = true,
                    textStyle = TextStyle(color = Palette.Text, fontSize = 28.sp, fontFamily = Plex, textAlign = TextAlign.Center,
                        textDirection = TextDirection.Ltr, letterSpacing = 8.sp),
                    cursorBrush = SolidColor(Palette.Glow),
                    keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(keyboardType = androidx.compose.ui.text.input.KeyboardType.NumberPassword),
                    modifier = Modifier.fillMaxWidth().height(60.dp).clip(shape).background(Palette.Soft)
                        .border(1.dp, if (Link.pinError) Palette.Danger else Palette.PanelBorder, shape).padding(12.dp),
                )
                if (Link.pinWait > 0) T("محاولات خاطئة كثيرة. انتظر ${Link.pinWait} ثانية ثم حاول مرة أخرى.", 13.sp, color = Palette.Danger)
                else if (Link.pinError) T("الرمز غير صحيح، حاول مرة أخرى.", 13.sp, color = Palette.Danger)
            }
        },
        confirmButton = {
            PrimaryButton("اتصال", null, pin.length >= 4, 44.dp, 15.sp, Modifier.width(110.dp)) { Link.sendPin(pin) }
        },
        dismissButton = { TextButton(onClick = { Link.dismissPin() }) { T("إلغاء", 15.sp, color = Palette.TextMuted) } },
    )
}

// ───────────────────────────── Control panel ─────────────────────────────

private class Tile(val label: String, val icon: Int, val top: Color, val bottom: Color, val confirm: String? = null, val action: () -> Unit)

private fun key(vk: Int) = { Link.send(Messages.action("key") { put("vk", vk) }) }
private fun combo(vararg vks: Int) = { Link.send(Messages.action("combo") { put("keys", org.json.JSONArray(vks.toList())) }) }
private fun open(app: String) = { Link.send(Messages.action("open") { put("app", app) }) }
private fun power(name: String) = { Link.send(Messages.action(name)) }

private val Blue = Color(0xFF1D58C9) to Color(0xFF0D2C80)
private val Teal = Color(0xFF13656E) to Color(0xFF0A3846)
private val Purple = Color(0xFF45338C) to Color(0xFF241A56)
private val Red = Color(0xFFB8403A) to Color(0xFF6E1F1C)
private val Green = Color(0xFF1E8A50) to Color(0xFF0E4A2B)

@Composable
private fun ColumnScope.ControlPage(ui: UiState, wide: Boolean) {
    val context = LocalContext.current
    var confirm by remember { mutableStateOf<Tile?>(null) }
    T("لوحة التحكم", if (wide) 30.sp else 24.sp, FontWeight.Bold)
    T("أزرار سريعة للكمبيوتر، تعمل مع العرض أو بدونه", 15.sp, color = Palette.TextMuted)

    // PC load
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_activity, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("أداء الكمبيوتر", 17.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
            if (!ui.connected) T("غير متصل", 13.sp, color = Palette.TextMuted)
        }
        Meter("المعالج", R.drawable.ic_cpu, Link.cpu, "${Link.cpu ?: 0}%")
        Meter("الذاكرة", R.drawable.ic_memory, Link.ram, Link.ramText.ifBlank { "${Link.ram ?: 0}%" })
    }

    VolumeMixer(ui)
    AirMouse(ui)

    val cols = if (wide) 6 else 3
    Section("الميديا والصوت", cols, listOf(
        Tile("تشغيل/إيقاف", R.drawable.ic_play_pause, Blue.first, Blue.second, action = key(VK.PLAY_PAUSE)),
        Tile("السابق", R.drawable.ic_skip_back, Blue.first, Blue.second, action = key(VK.PREV)),
        Tile("التالي", R.drawable.ic_skip_fwd, Blue.first, Blue.second, action = key(VK.NEXT)),
        Tile("خفض الصوت", R.drawable.ic_vol_down, Teal.first, Teal.second, action = key(VK.VOL_DOWN)),
        Tile("رفع الصوت", R.drawable.ic_sound, Teal.first, Teal.second, action = key(VK.VOL_UP)),
        Tile("كتم", R.drawable.ic_vol_mute, Teal.first, Teal.second, action = key(VK.VOL_MUTE)),
    ), ui.connected) { confirm = it }
    Section("البرامج", cols, listOf(
        Tile("المتصفح", R.drawable.ic_globe, Purple.first, Purple.second, action = open("browser")),
        Tile("المستكشف", R.drawable.ic_folder, Purple.first, Purple.second, action = open("explorer")),
        Tile("الآلة الحاسبة", R.drawable.ic_calc, Purple.first, Purple.second, action = open("calc")),
        Tile("المفكرة", R.drawable.ic_notepad, Purple.first, Purple.second, action = open("notepad")),
        Tile("الرسام", R.drawable.ic_palette, Purple.first, Purple.second, action = open("paint")),
        Tile("مدير المهام", R.drawable.ic_activity, Purple.first, Purple.second, action = open("taskmgr")),
    ), ui.connected) { confirm = it }
    Section("النوافذ والاختصارات", cols, listOf(
        Tile("سطح المكتب", R.drawable.ic_pc, Blue.first, Blue.second, action = combo(VK.WIN, VK.letter('D'))),
        Tile("تبديل النوافذ", R.drawable.ic_switch, Blue.first, Blue.second, action = combo(VK.ALT, VK.TAB)),
        Tile("لقطة إلى الجوال", R.drawable.ic_screenshot, Blue.first, Blue.second, action = power("screenshot")),
        Tile("نسخ", R.drawable.ic_copy, Teal.first, Teal.second, action = combo(VK.CTRL, VK.letter('C'))),
        Tile("لصق", R.drawable.ic_paste, Teal.first, Teal.second, action = combo(VK.CTRL, VK.letter('V'))),
        Tile("تراجع", R.drawable.ic_undo, Teal.first, Teal.second, action = combo(VK.CTRL, VK.letter('Z'))),
    ), ui.connected) { confirm = it }
    Section("العرض التقديمي والشرح", cols, listOf(
        Tile("بدء العرض", R.drawable.ic_show, Green.first, Green.second, action = key(0x74)),
        Tile("السابق", R.drawable.ic_prev, Green.first, Green.second, action = key(VK.LEFT)),
        Tile("التالي", R.drawable.ic_next, Green.first, Green.second, action = key(VK.RIGHT)),
        Tile("شاشة سوداء", R.drawable.ic_black, Green.first, Green.second, action = key(VK.letter('B'))),
        Tile("الرسم على الشاشة", R.drawable.ic_annotate, Purple.first, Purple.second,
            action = { Link.send(Messages.action("annotate") { put("mode", "toggle") }) }),
        Tile("مسح الرسم", R.drawable.ic_eraser, Purple.first, Purple.second,
            action = { Link.send(Messages.action("annotate") { put("mode", "clear") }) }),
    ), ui.connected) { confirm = it }
    PresenterTimer()
    Section("البث المباشر (OBS)", cols, listOf(
        Tile("المشهد 1", R.drawable.ic_show, Red.first, Red.second, action = combo(VK.CTRL, VK.SHIFT, 0x70)),
        Tile("المشهد 2", R.drawable.ic_show, Red.first, Red.second, action = combo(VK.CTRL, VK.SHIFT, 0x71)),
        Tile("المشهد 3", R.drawable.ic_show, Red.first, Red.second, action = combo(VK.CTRL, VK.SHIFT, 0x72)),
        Tile("بدء/إيقاف البث", R.drawable.ic_record, Red.first, Red.second, action = combo(VK.CTRL, VK.SHIFT, 0x73)),
        Tile("بدء/إيقاف التسجيل", R.drawable.ic_record, Red.first, Red.second, action = combo(VK.CTRL, VK.SHIFT, 0x74)),
        Tile("كتم الميكروفون", R.drawable.ic_vol_mute, Red.first, Red.second, action = combo(VK.CTRL, VK.SHIFT, 0x75)),
    ), ui.connected) { confirm = it }
    T("اربط هذه الأزرار في OBS: الإعدادات ← اختصارات المفاتيح، بالضغط عليها من هنا (Ctrl+Shift+F1 إلى F6).", 12.sp, color = Palette.TextMuted)
    Section("أدوات", cols, listOf(
        Tile("لون تحت المؤشر", R.drawable.ic_pipette, Teal.first, Teal.second, action = { Link.send(Messages.action("color")) }),
        Tile("إنهاء العرض", R.drawable.ic_close, Green.first, Green.second, action = key(VK.ESC)),
    ), ui.connected) { confirm = it }
    if (Link.pickedColor.isNotBlank()) {
        val hex = Link.pickedColor
        val color = runCatching { Color(android.graphics.Color.parseColor(hex)) }.getOrDefault(Color.Gray)
        AlertDialog(
            onDismissRequest = { Link.clearColor() },
            containerColor = Palette.Panel,
            title = { T("اللون تحت المؤشر", 19.sp, FontWeight.Bold) },
            text = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(64.dp).clip(RoundedCornerShape(14.dp)).background(color).border(1.dp, Palette.PanelBorder, RoundedCornerShape(14.dp)))
                    Spacer(Modifier.width(16.dp))
                    T(hex, 26.sp, FontWeight.Bold)
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    (context.getSystemService(android.content.Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager)
                        .setPrimaryClip(android.content.ClipData.newPlainText("color", hex))
                    Link.clearColor()
                }) { T("نسخ", 16.sp, FontWeight.Bold, Palette.Glow) }
            },
            dismissButton = { TextButton(onClick = { Link.clearColor() }) { T("إغلاق", 16.sp, color = Palette.TextMuted) } },
        )
    }
    Section("الطاقة", cols, listOf(
        Tile("قفل", R.drawable.ic_lock, Purple.first, Purple.second, action = power("lock")),
        Tile("سكون", R.drawable.ic_moon, Purple.first, Purple.second, confirm = "وضع الكمبيوتر في السكون؟", action = power("sleep")),
        Tile("إعادة التشغيل", R.drawable.ic_restart, Red.first, Red.second, confirm = "إعادة تشغيل الكمبيوتر؟ احفظ عملك أولًا.", action = power("restart")),
        Tile("إيقاف التشغيل", R.drawable.ic_power, Red.first, Red.second, confirm = "إيقاف تشغيل الكمبيوتر؟ احفظ عملك أولًا.", action = power("shutdown")),
    ), ui.connected) { confirm = it }

    // Wake-on-LAN (works while the PC is off or asleep)
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_zap, 22.dp, Palette.Warn)
            Spacer(Modifier.width(8.dp))
            T("تشغيل الكمبيوتر عن بُعد", 17.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
        }
        val mac = remember(Link.connected) { Prefs.load(context).mac }
        T(if (mac.isBlank()) "اتصل بالكمبيوتر مرة واحدة عبر Wi-Fi أو الكيبل ليتعرّف التطبيق على بطاقة الشبكة."
          else "يرسل إشارة إيقاظ إلى $mac على شبكة Wi-Fi نفسها. يجب تفعيل Wake-on-LAN في إعدادات BIOS وبطاقة الشبكة.",
          13.sp, color = Palette.TextMuted)
        PrimaryButton("تشغيل الكمبيوتر", R.drawable.ic_zap, mac.isNotBlank(), 48.dp, 16.sp, Modifier.fillMaxWidth()) {
            if (Link.wake(mac)) android.widget.Toast.makeText(context, "أُرسلت إشارة الإيقاظ", android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    // Files both ways
    val pick = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) FileTransfer.send(context, Link.currentHost, uris)
    }
    val scanTarget = remember {
        val f = java.io.File(java.io.File(context.cacheDir, "scans").apply { mkdirs() }, "camera.jpg")
        androidx.core.content.FileProvider.getUriForFile(context, context.packageName + ".files", f)
    }
    val camera = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.TakePicture()) { ok ->
        if (ok) FileTransfer.photoToPdf(context, scanTarget)?.let { FileTransfer.send(context, Link.currentHost, listOf(it)) }
    }
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_file_up, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("الملفات", 17.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
        }
        T("ما ترسله يصل إلى «التنزيلات\\Imtidad» في الكمبيوتر. ومن الكمبيوتر: زر الفأرة الأيمن على أي ملف ← إرسال إلى ← امتداد (إلى الجوال). وتجد «إرسال إلى الكمبيوتر» في قائمة المشاركة بأي تطبيق.",
            13.sp, color = Palette.TextMuted)
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PrimaryButton("إرسال ملف", R.drawable.ic_file_up, ui.connected, 48.dp, 15.sp, Modifier.weight(1f)) { pick.launch(arrayOf("*/*")) }
            OutlineButton("مسح مستند إلى PDF", R.drawable.ic_camera, Palette.PanelBorder, 48.dp, Modifier.weight(1f)) {
                if (ui.connected) camera.launch(scanTarget)
            }
        }
        FileTransfer.received.take(5).forEach { r ->
            Row(
                Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Palette.Soft).clickable {
                    runCatching {
                        context.startActivity(Intent(Intent.ACTION_VIEW).setDataAndType(r.uri, r.mime)
                            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK))
                    }
                }.padding(12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Ic(if (r.mime.startsWith("image/")) R.drawable.ic_image else R.drawable.ic_file, 22.dp)
                Spacer(Modifier.width(10.dp))
                T(r.name, 14.sp, modifier = Modifier.weight(1f), maxLines = 1)
                T("فتح", 14.sp, FontWeight.Bold, Palette.Glow)
            }
        }
    }

    // Shared clipboard
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_clipboard, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("الحافظة المشتركة", 17.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
        }
        T("ما تنسخه على الكمبيوتر يصل إلى جهازك تلقائيًا، وما تنسخه هنا يصل إلى الكمبيوتر.", 13.sp, color = Palette.TextMuted)
        if (Link.lastClip.isNotBlank()) {
            T("آخر نص: " + Link.lastClip.take(120), 14.sp, maxLines = 2)
        }
        OutlineButton("إرسال حافظة الجهاز الآن", R.drawable.ic_send, Palette.PanelBorder, 46.dp, Modifier.fillMaxWidth()) {
            val ok = Link.sendClipboard()
            android.widget.Toast.makeText(context, if (ok) "أُرسل إلى الكمبيوتر" else "لا يوجد نص في الحافظة", android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    confirm?.let { tile ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            containerColor = Palette.Panel,
            title = { T(tile.label, 20.sp, FontWeight.Bold) },
            text = { T(tile.confirm ?: "", 15.sp, color = Palette.TextMuted) },
            confirmButton = { TextButton(onClick = { tile.action(); confirm = null }) { T("نعم", 16.sp, FontWeight.Bold, Palette.Danger) } },
            dismissButton = { TextButton(onClick = { confirm = null }) { T("إلغاء", 16.sp, color = Palette.TextMuted) } },
        )
    }
}

@Composable
private fun Meter(label: String, icon: Int, value: Int?, text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Ic(icon, 20.dp, Palette.TextMuted)
        Spacer(Modifier.width(8.dp))
        T(label, 14.sp, modifier = Modifier.width(70.dp))
        val v = (value ?: 0).coerceIn(0, 100) / 100f
        val color = when { v > 0.85f -> Palette.Danger; v > 0.6f -> Palette.Warn; else -> Palette.Good }
        Box(Modifier.weight(1f).height(10.dp).clip(RoundedCornerShape(5.dp)).background(Palette.Soft)) {
            Box(Modifier.fillMaxHeight().fillMaxWidth(v).clip(RoundedCornerShape(5.dp)).background(color))
        }
        Spacer(Modifier.width(10.dp))
        T(if (value == null) "—" else text, 13.sp, color = Palette.TextMuted, modifier = Modifier.widthIn(min = 70.dp))
    }
}

@Composable
private fun Section(title: String, cols: Int, tiles: List<Tile>, enabled: Boolean, onConfirm: (Tile) -> Unit) {
    T(title, 17.sp, FontWeight.Bold)
    tiles.chunked(cols).forEach { row ->
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            row.forEach { t ->
                val shape = RoundedCornerShape(16.dp)
                Column(
                    Modifier.weight(1f).height(96.dp).graphicsLayer { alpha = if (enabled) 1f else 0.4f }.clip(shape)
                        .background(Palette.Panel)
                        .border(1.dp, Palette.PanelBorder, shape)
                        .clickable(enabled = enabled) { if (t.confirm != null) onConfirm(t) else t.action() },
                    horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center,
                ) {
                    Box(Modifier.size(40.dp).clip(RoundedCornerShape(12.dp)).background(t.top), contentAlignment = Alignment.Center) {
                        Ic(t.icon, 22.dp, Color.White)
                    }
                    Spacer(Modifier.height(8.dp))
                    T(t.label, 13.sp, FontWeight.SemiBold, maxLines = 1)
                }
            }
            repeat(cols - row.size) { Spacer(Modifier.weight(1f)) }
        }
    }
}

// ───────────────────────────── One window ─────────────────────────────

/** "What to show": the whole screen or one PC window (mirror / remote modes). */
@Composable
private fun WindowChoice(ui: UiState) {
    if (ui.prefs.mode == "EXTEND") return
    var open by remember { mutableStateOf(false) }
    val p = ui.prefs
    // One compact line: what will be shown, and a button to pick one window.
    Row(Modifier.fillMaxWidth().height(40.dp), verticalAlignment = Alignment.CenterVertically) {
        Ic(R.drawable.ic_pc, 18.dp, Palette.TextMuted)
        Spacer(Modifier.width(8.dp))
        T(if (p.windowId == 0L) "يُعرض: الشاشة كاملة" else "يُعرض: نافذة «${p.windowTitle}»", 14.sp,
            color = Palette.TextMuted, modifier = Modifier.weight(1f), maxLines = 1)
        if (p.windowId != 0L) {
            TextButton(onClick = { ui.update(p.copy(windowId = 0, windowTitle = "")) }) { T("الشاشة كاملة", 14.sp, color = Palette.Glow) }
        }
        TextButton(enabled = ui.connected, onClick = { Link.requestWindows(); open = true }) { T("اختيار نافذة", 14.sp, FontWeight.Bold, Palette.Glow) }
    }
    if (open) WindowDialog(ui) { open = false }
}

/** List of the PC's open windows; choosing one shows it alone. */
@Composable
private fun WindowDialog(ui: UiState, onClose: () -> Unit) {
    val p = ui.prefs
    AlertDialog(
        onDismissRequest = onClose,
        containerColor = Palette.Panel,
        title = { T("اختر نافذة لعرضها وحدها", 19.sp, FontWeight.Bold) },
        text = {
            Column(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (Link.windows.isEmpty()) T("جارٍ جلب النوافذ المفتوحة…", 14.sp, color = Palette.TextMuted)
                Link.windows.forEach { (id, title) ->
                    Row(
                        Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(Palette.Soft)
                            .clickable { ui.update(p.copy(windowId = id, windowTitle = title.take(60))); onClose() }.padding(12.dp),
                    ) { T(title, 14.sp, maxLines = 1) }
                }
                if (p.windowId != 0L) {
                    TextButton(onClick = { ui.update(p.copy(windowId = 0, windowTitle = "")); onClose() }) {
                        T("الشاشة كاملة", 14.sp, FontWeight.Bold, Palette.Glow)
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = onClose) { T("إغلاق", 15.sp, color = Palette.TextMuted) } },
    )
}

/** A stopwatch for talks: start / pause / reset. */
@Composable
private fun PresenterTimer() {
    var running by remember { mutableStateOf(false) }
    var elapsed by remember { mutableLongStateOf(0L) }
    LaunchedEffect(running) {
        var last = System.currentTimeMillis()
        while (running) {
            delay(250)
            val now = System.currentTimeMillis()
            elapsed += now - last
            last = now
        }
    }
    Glass(Modifier.fillMaxWidth(), padding = 14.dp) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_timer, 24.dp)
            Spacer(Modifier.width(10.dp))
            T("وقت العرض", 16.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
            val sec = elapsed / 1000
            T("%02d:%02d".format(sec / 60, sec % 60), 28.sp, FontWeight.Bold)
            Spacer(Modifier.width(16.dp))
            TextButton(onClick = { running = !running }) { T(if (running) "إيقاف مؤقت" else "ابدأ", 15.sp, FontWeight.Bold, Palette.Glow) }
            TextButton(onClick = { running = false; elapsed = 0 }) { T("تصفير", 15.sp, color = Palette.TextMuted) }
        }
    }
}

@Composable
private fun PenSettings(ui: UiState) {
    val p = ui.prefs
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_pen, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("القلم", 20.sp, FontWeight.Bold)
        }
        T("إحساس الضغط", 14.sp, color = Palette.TextMuted)
        Segmented(listOf("خفيف", "عادي", "ثابت"), p.pressure) { ui.update(p.copy(pressure = it)) }
        T("خفيف: لمسة خفيفة تعطي خطًا قويًا. ثابت: يحتاج ضغطًا أكبر.", 13.sp, color = Palette.TextMuted)
    }
}

// ───────────────────────────── Volume mixer ─────────────────────────────

@Composable
private fun VolumeMixer(ui: UiState) {
    LaunchedEffect(ui.connected) { if (ui.connected) Link.requestVolumes() }
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_sound, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("مستوى الصوت", 17.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
            TextButton(enabled = ui.connected, onClick = { Link.requestVolumes() }) { T("تحديث", 14.sp, color = Palette.Glow) }
        }
        Link.master?.let { (v, m) -> VolumeRow("الكمبيوتر (الكل)", v, m, bold = true) { nv, nm -> Link.setVolume(0, nv, nm) } }
        Link.appVolumes.forEach { a -> VolumeRow(a.name, a.v, a.muted) { nv, nm -> Link.setVolume(a.pid, nv, nm) } }
        if (Link.master == null && ui.connected) T("جارٍ القراءة…", 13.sp, color = Palette.TextMuted)
        if (Link.master != null && Link.appVolumes.isEmpty()) T("لا توجد برامج تشغّل صوتًا الآن.", 13.sp, color = Palette.TextMuted)
    }
}

@Composable
private fun VolumeRow(name: String, v: Float, muted: Boolean, bold: Boolean = false, onChange: (Float?, Boolean?) -> Unit) {
    var value by remember(v) { mutableFloatStateOf(v) }
    Row(verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = { onChange(null, !muted) }) {
            Ic(if (muted) R.drawable.ic_vol_mute else R.drawable.ic_sound, 22.dp, if (muted) Palette.Danger else Palette.Text)
        }
        T(name, 14.sp, if (bold) FontWeight.Bold else FontWeight.Normal, modifier = Modifier.width(140.dp), maxLines = 1)
        Slider(
            value = value, onValueChange = { value = it }, onValueChangeFinished = { onChange(value, null) },
            modifier = Modifier.weight(1f),
            colors = SliderDefaults.colors(thumbColor = Palette.Accent, activeTrackColor = Palette.Accent, inactiveTrackColor = Palette.Soft),
        )
        T("${(value * 100).toInt()}%", 13.sp, color = Palette.TextMuted, modifier = Modifier.width(48.dp))
    }
}

// ───────────────────────────── Air mouse ─────────────────────────────

/** Move the PC pointer by tilting/turning the device (gyroscope); big buttons click. */
@Composable
private fun AirMouse(ui: UiState) {
    val context = LocalContext.current
    var on by remember { mutableStateOf(false) }
    DisposableEffect(on) {
        val sm = context.getSystemService(android.content.Context.SENSOR_SERVICE) as android.hardware.SensorManager
        val gyro = sm.getDefaultSensor(android.hardware.Sensor.TYPE_GYROSCOPE)
        var remX = 0f
        var remY = 0f
        val listener = object : android.hardware.SensorEventListener {
            override fun onSensorChanged(e: android.hardware.SensorEvent) {
                // Turning left/right (z) moves across, tilting up/down (x) moves vertically.
                val dx = -e.values[2] * 22f + remX
                val dy = -e.values[0] * 22f + remY
                val ix = dx.toInt()
                val iy = dy.toInt()
                remX = dx - ix; remY = dy - iy
                if (kotlin.math.abs(ix) + kotlin.math.abs(iy) >= 1) Link.input(InputMessages.rel(ix, iy))
            }
            override fun onAccuracyChanged(s: android.hardware.Sensor?, a: Int) {}
        }
        if (on && gyro != null) sm.registerListener(listener, gyro, android.hardware.SensorManager.SENSOR_DELAY_GAME)
        onDispose { sm.unregisterListener(listener) }
    }
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_mouse, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("الماوس الهوائي", 17.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
            AudioSwitch(on && ui.connected) { on = it }
        }
        T("حرّك الجهاز في الهواء ليتحرك مؤشر الكمبيوتر. مفيد للعروض والتحكم من بعيد.", 13.sp, color = Palette.TextMuted)
        if (on) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                PrimaryButton("نقرة", null, ui.connected, 56.dp, 17.sp, Modifier.weight(1f)) { Link.input(InputMessages.button("l", "c")) }
                OutlineButton("نقرة يمين", null, Palette.PanelBorder, 56.dp, Modifier.weight(1f)) { Link.input(InputMessages.button("r", "c")) }
            }
        }
    }
}

// ───────────────────────────── Profiles ─────────────────────────────

private data class Profile(val name: String, val icon: Int, val apply: (Prefs) -> Prefs)

private val Profiles = listOf(
    Profile("عمل", R.drawable.ic_extend) { it.copy(mode = "EXTEND", fps = 60, quality = 1, fullResolution = true, drawTools = false, night = false, touchpad = false) },
    Profile("رسم", R.drawable.ic_pen) { it.copy(mode = "EXTEND", fps = 60, quality = 2, fullResolution = true, drawTools = true, penOnly = true, touchpad = false) },
    Profile("أفلام", R.drawable.ic_play) { it.copy(mode = "MIRROR", fps = 60, quality = 2, fullResolution = true, audio = true, drawTools = false) },
    Profile("بطارية", R.drawable.ic_zap) { it.copy(fps = 30, quality = 0, fullResolution = false) },
)

@Composable
private fun ProfileChips(ui: UiState) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        T("إعدادات جاهزة:", 14.sp, color = Palette.TextMuted)
        Profiles.forEach { pr ->
            val shape = RoundedCornerShape(50)
            Row(
                Modifier.clip(shape).background(Palette.Soft).border(1.dp, Palette.PanelBorder, shape)
                    .clickable { ui.update(pr.apply(ui.prefs)) }.padding(horizontal = 14.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Ic(pr.icon, 16.dp)
                Spacer(Modifier.width(6.dp))
                T(pr.name, 14.sp, FontWeight.SemiBold)
            }
        }
    }
}

// ───────────────────────────── First run ─────────────────────────────

@Composable
private fun Onboarding(onDone: () -> Unit) {
    val pages = listOf(
        Triple(R.drawable.ic_usb, "صِل الجهاز بالكمبيوتر", "شغّل برنامج امتداد على الكمبيوتر، ثم صِل الجهاز بالكيبل (مع تفعيل تصحيح USB)، أو اختر الكمبيوتر من الإعدادات عبر Wi-Fi."),
        Triple(R.drawable.ic_extend, "اختر ما تريد", "انعكاس الشاشة، أو شاشة ثانية، أو تحكم عن بعد، أو لوح رسم للقلم. ثم اضغط «بدء العرض»."),
        Triple(R.drawable.ic_control, "لوحة التحكم", "أزرار للصوت والبرامج والطاقة، والملفات واللقطات، والحافظة المشتركة، والماوس الهوائي."),
    )
    var page by remember { mutableIntStateOf(0) }
    AlertDialog(
        onDismissRequest = onDone,
        containerColor = Palette.Panel,
        icon = { Ic(pages[page].first, 48.dp, Palette.Glow) },
        title = { T(pages[page].second, 22.sp, FontWeight.Bold, align = TextAlign.Center) },
        text = {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(14.dp)) {
                T(pages[page].third, 15.sp, color = Palette.TextMuted, align = TextAlign.Center)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    pages.indices.forEach { i -> Dot(if (i == page) Palette.Glow else Palette.PanelBorder, 8.dp) }
                }
            }
        },
        confirmButton = {
            PrimaryButton(if (page < pages.size - 1) "التالي" else "ابدأ", null, true, 44.dp, 15.sp, Modifier.width(120.dp)) {
                if (page < pages.size - 1) page++ else onDone()
            }
        },
        dismissButton = { TextButton(onClick = onDone) { T("تخطي", 15.sp, color = Palette.TextMuted) } },
    )
}

// ───────────────────────────── Diagnostics ─────────────────────────────

@Composable
private fun Diagnostics(ui: UiState) {
    var result by remember { mutableStateOf<String?>(null) }
    var running by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    Glass(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Ic(R.drawable.ic_activity, 22.dp)
            Spacer(Modifier.width(8.dp))
            T("فحص الاتصال", 20.sp, FontWeight.Bold, modifier = Modifier.weight(1f))
            PrimaryButton(if (running) "جارٍ الفحص…" else "افحص", null, ui.connected && !running, 40.dp, 14.sp, Modifier.width(130.dp)) {
                running = true
                scope.launch {
                    val mbps = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { FileTransfer.speedTest(Link.currentHost) }
                    val lat = Link.latencyMs
                    val via = if (Link.currentHost == "127.0.0.1") "USB" else "Wi-Fi"
                    val advice = when {
                        mbps == null -> "تعذّر قياس السرعة."
                        mbps > 150 -> "ممتاز: استخدم الدقة الأصلية و60 أو 120 إطارًا وجودة عالية."
                        mbps > 60 -> "جيد: الدقة الأصلية و60 إطارًا وجودة متوسطة إلى عالية."
                        mbps > 25 -> "متوسط: 30 إطارًا وجودة متوسطة، أو نصف الدقة."
                        else -> "ضعيف: نصف الدقة و30 إطارًا وجودة منخفضة، أو استخدم الكيبل."
                    }
                    result = "الاتصال: $via\nزمن الاستجابة: ${lat ?: "—"}ms\nالسرعة: ${mbps?.let { "%.0f Mbps".format(it) } ?: "—"}\n$advice"
                    running = false
                }
            }
        }
        T("يقيس السرعة بين الجهازين ويقترح أفضل إعدادات.", 13.sp, color = Palette.TextMuted)
        result?.let { T(it, 15.sp) }
    }
}
