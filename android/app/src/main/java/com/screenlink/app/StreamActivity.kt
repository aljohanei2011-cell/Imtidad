package com.screenlink.app

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.clickable
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.waitForUpOrCancellation
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/** Full-screen view of the PC desktop, with touch control and a small toolbar. */
class StreamActivity : ComponentActivity() {
    override fun onResume() {
        super.onResume()
        ThemeChoice.forceDark = true   // the stream screen keeps the dark look around the picture
    }

    override fun onPause() {
        super.onPause()
        ThemeChoice.forceDark = false
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        val prefs = Prefs.load(this)
        ThemeChoice.forceDark = true
        setContent {
            ScreenLinkTheme(systemBars = false) {
                StreamScreen(
                    prefs = prefs,
                    onExit = { finish() },
                    onRotate = {
                        requestedOrientation =
                            if (requestedOrientation == ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT) {
                                ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                            } else {
                                ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
                            }
                    },
                )
            }
        }
    }
}

@Composable
fun StreamScreen(prefs: Prefs, onExit: () -> Unit, onRotate: () -> Unit) {
    var videoW by remember { mutableIntStateOf(0) }
    var videoH by remember { mutableIntStateOf(0) }
    var stats by remember { mutableStateOf("جارٍ الاتصال…") }
    var codecName by remember { mutableStateOf("") }
    var error by remember { mutableStateOf<String?>(null) }
    var client by remember { mutableStateOf<VideoClient?>(null) }
    // Reconnecting after the stream drops (Wi-Fi gone for a moment, cable pulled, PC asleep).
    var dropped by remember { mutableStateOf(false) }
    var reconnecting by remember { mutableStateOf(false) }
    var lastError by remember { mutableStateOf<String?>(null) }
    val dropSince = remember { longArrayOf(0L) }          // when the picture stopped (0 = it is running)
    val everStreamed = remember { booleanArrayOf(false) }  // only a stream that worked is retried by itself
    val surfaceRef = remember { arrayOfNulls<android.view.Surface>(1) }
    val dimsRef = remember { arrayOf(0 to 0) }
    // Stopped from the PC: close the stream screen rather than reconnect.
    val stopsAtStart = remember { Link.pcStoppedStream }
    LaunchedEffect(Link.pcStoppedStream) { if (Link.pcStoppedStream != stopsAtStart) onExit() }
    var toolbarVisible by remember { mutableStateOf(true) }
    val context0 = LocalContext.current
    var options by remember { mutableStateOf(Prefs.load(context0)) }
    fun setOptions(p: Prefs) { options = p; p.save(context0) }
    val touchRef = remember { arrayOfNulls<TouchInput>(1) }
    var lensOn by remember { mutableStateOf(false) }
    var recording by remember { mutableStateOf(false) }
    var dialOn by remember { mutableStateOf(false) }
    LaunchedEffect(options.touchpad, options.penOnly, lensOn) {
        touchRef[0]?.touchpad = options.touchpad
        touchRef[0]?.penOnly = options.penOnly
        touchRef[0]?.lens = lensOn
    }
    DisposableEffect(Unit) {
        Link.streaming = true
        val started = System.currentTimeMillis()
        onDispose {
            Link.streaming = false
            addUsage(context0, (System.currentTimeMillis() - started) / 1000)
        }
    }
    // Drawing programs in front on the PC bring the drawing tools up by themselves.
    val drawingApps = listOf("photoshop", "krita", "illustrator", "clipstudio", "paint", "mspaint", "gimp", "sai", "affinity",
        "inkscape", "blender", "zbrush", "onenote", "xournal", "medibang", "rebelle", "corel", "autocad", "figma")
    val autoDraw = options.autoTools && drawingApps.any { Link.foregroundExe.contains(it) }
    var panel by remember { mutableStateOf(Panel.None) }
    val density = LocalDensity.current.density
    val context = LocalContext.current
    // Real panel size in landscape (the stream screen is landscape; system bars are hidden).
    val screen = remember {
        val real = android.graphics.Point()
        val wm = context.getSystemService(android.content.Context.WINDOW_SERVICE) as android.view.WindowManager
        @Suppress("DEPRECATION")
        wm.defaultDisplay.getRealSize(real)
        maxOf(real.x, real.y) to minOf(real.x, real.y)
    }

    val listener = remember {
        object : VideoClient.Listener {
            override fun onVideoSize(width: Int, height: Int) {
                videoW = width; videoH = height
                everStreamed[0] = true; dropSince[0] = 0L; reconnecting = false
            }
            override fun onStats(fps: Int, kbps: Int) {
                if (fps > 0) { everStreamed[0] = true; dropSince[0] = 0L; reconnecting = false }
                stats = "$fps إطار/ث  ·  ${"%.0f".format(kbps / 1000.0)} Mbps" + if (codecName.isNotEmpty()) "  ·  $codecName" else ""
            }
            override fun onCodec(hevc: Boolean) { codecName = if (hevc) "H.265" else "H.264" }
            override fun onError(message: String) { lastError = message }
            override fun onClosed() { stats = "غير متصل"; dropped = true }
        }
    }
    val send: (String) -> Unit = { msg -> client?.sendInput(msg) }
    val audioRef = remember { arrayOfNulls<AudioClient>(1) }
    DisposableEffect(Unit) {
        audioRef[0] = if (prefs.audio) AudioClient(prefs.host).also { it.start() } else null
        onDispose {
            client?.stop()
            audioRef[0]?.stop()
        }
    }

    // The stream ended without the user closing it: try again every 2 s for up to a minute,
    // once the control link to the PC is back. A first connection that never worked shows the error at once.
    LaunchedEffect(dropped) {
        if (!dropped) return@LaunchedEffect
        if (!everStreamed[0]) {
            error = lastError ?: "لم يصل أي بث من الكمبيوتر."
            dropped = false
            return@LaunchedEffect
        }
        if (dropSince[0] == 0L) dropSince[0] = System.currentTimeMillis()
        reconnecting = true
        while (System.currentTimeMillis() - dropSince[0] < 60_000) {
            kotlinx.coroutines.delay(2000)
            val surface = surfaceRef[0] ?: continue
            if (!Link.connected) continue
            client?.stop()
            val c = VideoClient(prefs.host, Prefs.load(context0).streamConfig(dimsRef[0].first, dimsRef[0].second), listener)
            client = c
            audioRef[0]?.stop()
            audioRef[0] = if (prefs.audio) AudioClient(prefs.host).also { it.start() } else null
            c.start(surface)
            dropped = false   // if this try fails too, onClosed sets it again and the loop goes on
            return@LaunchedEffect
        }
        reconnecting = false
        dropSince[0] = 0L
        error = lastError ?: "انقطع الاتصال بالكمبيوتر ولم يرجع خلال دقيقة."
        dropped = false
    }

    // Tablet held upright in second-screen mode: ask the PC for an upright screen (stream restarts).
    val portrait = androidx.compose.ui.platform.LocalConfiguration.current.orientation ==
        android.content.res.Configuration.ORIENTATION_PORTRAIT
    val dims = if (portrait && prefs.mode == "EXTEND") screen.second to screen.first else screen
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        val ratio = if (videoW > 0 && videoH > 0) videoW.toFloat() / videoH else 16f / 9f
        key(portrait) {
        AndroidView(
            factory = { ctx ->
                SurfaceView(ctx).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            client?.stop()
                            error = null
                            surfaceRef[0] = holder.surface
                            dimsRef[0] = dims
                            val c = VideoClient(prefs.host, Prefs.load(context).streamConfig(dims.first, dims.second), listener)
                            client = c
                            c.start(holder.surface)
                        }
                        override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            surfaceRef[0] = null
                            client?.stop()
                            client = null
                        }
                    })
                    val touch = TouchInput({ msg -> send(msg) }, density).also {
                        it.touchpad = options.touchpad
                        it.penOnly = options.penOnly
                        it.lens = lensOn
                        it.pressureGamma = listOf(0.6f, 1f, 1.6f)[options.pressure.coerceIn(0, 2)]
                        touchRef[0] = it
                    }
                    setOnTouchListener(touch)
                    setOnGenericMotionListener(touch)  // pen hover
                }
            },
            modifier = Modifier.align(Alignment.Center).aspectRatio(ratio),
        )
        }

        if (prefs.mode == "DRAWPAD") {
            // Drawing pad: no picture, a calm grid. The pen draws on the PC's main screen.
            Canvas(Modifier.align(Alignment.Center).aspectRatio(ratio).fillMaxSize()) {
                val step = size.minDimension / 12
                var x = 0f
                while (x < size.width) { drawLine(Color(0x2245649E), androidx.compose.ui.geometry.Offset(x, 0f), androidx.compose.ui.geometry.Offset(x, size.height)); x += step }
                var y = 0f
                while (y < size.height) { drawLine(Color(0x2245649E), androidx.compose.ui.geometry.Offset(0f, y), androidx.compose.ui.geometry.Offset(size.width, y)); y += step }
                drawRect(Color(0x6659B8FF), style = androidx.compose.ui.graphics.drawscope.Stroke(4f))
            }
            Text("لوح الرسم — القلم يرسم على شاشة الكمبيوتر", color = Palette.TextMuted, fontSize = 15.sp,
                modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 20.dp))
        }
        if (options.night) {
            // Warm tint for evening work; it does not take touches.
            Box(Modifier.fillMaxSize().background(Color(0x33FF8A00)))
        }
        if (options.drawTools || autoDraw) {
            DrawTools(send, options.penOnly, dialOn, { dialOn = !dialOn }, Modifier.align(Alignment.CenterEnd).padding(end = 12.dp)) {
                setOptions(options.copy(penOnly = !options.penOnly))
            }
            if (dialOn) Dial(send, Modifier.align(Alignment.BottomStart).padding(24.dp))
        }
        if (toolbarVisible) {
            Toolbar(
                stats = stats,
                options = options,
                onOption = { setOptions(it) },
                lens = lensOn,
                onLens = { lensOn = !lensOn },
                recording = recording,
                onRecord = {
                    if (!recording) {
                        recording = client?.startRecording(context) == true
                        if (recording) android.widget.Toast.makeText(context, "بدأ التسجيل", android.widget.Toast.LENGTH_SHORT).show()
                    } else {
                        val name = client?.stopRecording()
                        recording = false
                        android.widget.Toast.makeText(context, if (name != null) "حُفظ في الفيديو/Imtidad: $name" else "لم يُسجَّل شيء",
                            android.widget.Toast.LENGTH_LONG).show()
                    }
                },
                modifier = Modifier.align(Alignment.TopCenter).padding(top = 12.dp),
                onKeyboard = { panel = if (panel == Panel.Keyboard) Panel.None else Panel.Keyboard },
                onShortcuts = { panel = if (panel == Panel.Shortcuts) Panel.None else Panel.Shortcuts },
                onRotate = onRotate,
                onHide = { toolbarVisible = false; panel = Panel.None },
                onExit = onExit,
            )
        } else {
            Box(
                Modifier.align(Alignment.TopCenter).padding(top = 6.dp).glass(14)
                    .clickable { toolbarVisible = true }.padding(horizontal = 18.dp, vertical = 6.dp),
            ) { Text("الأدوات ▾", color = Color.White, fontSize = 14.sp) }
        }

        when (panel) {
            Panel.Keyboard -> KeyboardTabs(send, Modifier.align(Alignment.BottomCenter))
            Panel.Shortcuts -> ShortcutsPanel(send, Modifier.align(Alignment.BottomCenter))
            Panel.None -> {}
        }

        if (reconnecting && error == null) {
            Card(
                Modifier.align(Alignment.Center).padding(24.dp).widthIn(max = 420.dp),
                colors = CardDefaults.cardColors(containerColor = Palette.Panel),
            ) {
                Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("انقطع الاتصال", style = MaterialTheme.typography.titleLarge, color = Palette.Text)
                    Text("جارٍ إعادة الاتصال بالكمبيوتر…", color = Palette.TextMuted)
                    LinearProgressIndicator(Modifier.fillMaxWidth(), color = Palette.Glow, trackColor = Palette.PanelBorder)
                    TextButton(onClick = onExit) { Text("رجوع", color = Palette.TextMuted) }
                }
            }
        }

        error?.let { message ->
            Card(
                Modifier.align(Alignment.Center).padding(24.dp).widthIn(max = 520.dp),
                colors = CardDefaults.cardColors(containerColor = Palette.Panel),
            ) {
                Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text("تعذّر العرض", style = MaterialTheme.typography.titleLarge, color = Palette.Text)
                    Text(message, color = Palette.TextMuted)
                    Text(
                        "تأكد أن برنامج امتداد يعمل على الكمبيوتر، وأن الكيبل موصول (أو أن الجهازين على نفس الشبكة).",
                        color = Palette.TextMuted,
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Button(onClick = {
                            // Try again now, for another minute.
                            error = null
                            lastError = null
                            everStreamed[0] = true
                            dropSince[0] = 0L
                            dropped = true
                        }) { Text("إعادة المحاولة") }
                        OutlinedButton(onClick = onExit) { Text("رجوع") }
                    }
                }
            }
        }
    }
}

private enum class Panel { None, Keyboard, Shortcuts }

private val GlassBg = Brush.verticalGradient(listOf(Color(0xE6102660), Color(0xE60A1A4A)))
private val GlassBorder = Brush.verticalGradient(listOf(Color(0x8045649E), Color(0x4022357A)))

private fun Modifier.glass(radius: Int = 16): Modifier {
    val shape = RoundedCornerShape(radius.dp)
    return this.clip(shape).background(GlassBg).border(1.dp, GlassBorder, shape)
}

@Composable
private fun Toolbar(
    stats: String,
    options: Prefs,
    onOption: (Prefs) -> Unit,
    lens: Boolean,
    onLens: () -> Unit,
    recording: Boolean,
    onRecord: () -> Unit,
    modifier: Modifier,
    onKeyboard: () -> Unit,
    onShortcuts: () -> Unit,
    onRotate: () -> Unit,
    onHide: () -> Unit,
    onExit: () -> Unit,
) {
    BoxWithConstraints(modifier) {
    val wide = maxWidth >= 1000.dp
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        // Speed and quality of the stream.
        if (wide) Row(
            Modifier.height(58.dp).glass().padding(horizontal = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SignalBars()
            Spacer(Modifier.width(10.dp))
            Text(stats, color = Palette.Text, fontSize = 13.sp, fontWeight = FontWeight.Medium)
        }
        Row(
            Modifier.height(58.dp).glass().padding(horizontal = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ToolButton("لوحة المفاتيح", R.drawable.ic_keyboard, onKeyboard)
            ToolDivider()
            ToolButton("اختصارات", R.drawable.ic_shortcuts, onShortcuts)
            ToolDivider()
            ToolButton("تدوير", R.drawable.ic_rotate, onRotate)
            ToolDivider()
            ToolToggle("لوحة اللمس", R.drawable.ic_mouse, options.touchpad) { onOption(options.copy(touchpad = !options.touchpad)) }
            ToolToggle("أدوات الرسم", R.drawable.ic_pen, options.drawTools) { onOption(options.copy(drawTools = !options.drawTools)) }
            ToolToggle("ليلي", R.drawable.ic_moon, options.night) { onOption(options.copy(night = !options.night)) }
            ToolToggle("عدسة", R.drawable.ic_search, lens, onLens)
            ToolToggle(if (recording) "إيقاف" else "تسجيل", R.drawable.ic_record, recording, onRecord)
            ToolDivider()
            ToolButton("إخفاء", R.drawable.ic_hide, onHide)
            Spacer(Modifier.width(8.dp))
            Row(
                Modifier.height(44.dp).clip(RoundedCornerShape(12.dp))
                    .background(Brush.verticalGradient(listOf(Color(0xFFF0645B), Color(0xFFC7372F))))
                    .clickable(onClick = onExit).padding(horizontal = 18.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(painterResource(R.drawable.ic_close), contentDescription = null, tint = Color.White, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(6.dp))
                Text("إنهاء", color = Color.White, fontWeight = FontWeight.Bold, fontSize = 16.sp)
            }
        }
    }
}
}

@Composable
private fun ToolToggle(label: String, icon: Int, on: Boolean, onClick: () -> Unit) {
    Column(
        Modifier.clip(RoundedCornerShape(10.dp))
            .then(if (on) Modifier.background(Brush.verticalGradient(listOf(Color(0xFF3AA2FF), Color(0xFF1668E8)))) else Modifier)
            .clickable(onClick = onClick).padding(horizontal = 12.dp, vertical = 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(painterResource(icon), contentDescription = label, tint = Color.White, modifier = Modifier.size(22.dp))
        Spacer(Modifier.height(2.dp))
        Text(label, color = Color.White, fontSize = 12.sp)
    }
}

/** Keys to hold with one hand while drawing with the pen in the other (Photoshop, Krita, …). */
@Composable
private fun DrawTools(send: (String) -> Unit, penOnly: Boolean, dialOn: Boolean, onDial: () -> Unit, modifier: Modifier, onPenOnly: () -> Unit) {
    Column(modifier.glass(18).padding(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        HoldKey("Ctrl", VK.CTRL, send)
        HoldKey("Shift", VK.SHIFT, send)
        HoldKey("Alt", VK.ALT, send)
        HoldKey("مسافة", VK.SPACE, send)
        val dshape = RoundedCornerShape(12.dp)
        Box(
            Modifier.size(width = 76.dp, height = 52.dp).clip(dshape)
                .background(if (dialOn) Brush.verticalGradient(listOf(Color(0xFF3AA2FF), Color(0xFF1668E8)))
                            else Brush.verticalGradient(listOf(Color(0xFF15306F), Color(0xFF0C1F52))))
                .border(1.dp, Color(0xFF2E4F92), dshape).clickable(onClick = onDial),
            contentAlignment = Alignment.Center,
        ) { Text("العجلة", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold) }
        KeyBox(Modifier.size(width = 76.dp, height = 52.dp), onClick = { send(InputMessages.combo(VK.CTRL, VK.letter('Z'))) }) {
            Text("تراجع", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        }
        val shape = RoundedCornerShape(12.dp)
        Box(
            Modifier.size(width = 76.dp, height = 52.dp).clip(shape)
                .background(if (penOnly) Brush.verticalGradient(listOf(Color(0xFF3AA2FF), Color(0xFF1668E8)))
                            else Brush.verticalGradient(listOf(Color(0xFF15306F), Color(0xFF0C1F52))))
                .border(1.dp, Color(0xFF2E4F92), shape).clickable(onClick = onPenOnly),
            contentAlignment = Alignment.Center,
        ) { Text("القلم فقط", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold, textAlign = TextAlign.Center) }
    }
}

/** Held down while your finger is on it. */
@Composable
private fun HoldKey(label: String, vk: Int, send: (String) -> Unit) {
    var down by remember { mutableStateOf(false) }
    val shape = RoundedCornerShape(12.dp)
    Box(
        Modifier.size(width = 76.dp, height = 52.dp).clip(shape)
            .background(if (down) Brush.verticalGradient(listOf(Color(0xFF3AA2FF), Color(0xFF1668E8)))
                        else Brush.verticalGradient(listOf(Color(0xFF15306F), Color(0xFF0C1F52))))
            .border(1.dp, Color(0xFF2E4F92), shape)
            .pointerInput(vk) {
                awaitEachGesture {
                    awaitFirstDown()
                    down = true
                    send(InputMessages.keyDown(vk))
                    waitForUpOrCancellation()
                    down = false
                    send(InputMessages.keyUp(vk))
                }
            },
        contentAlignment = Alignment.Center,
    ) { Text(label, color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.SemiBold) }
}

@Composable
private fun SignalBars() {
    Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(2.dp)) {
        listOf(6, 9, 12, 15).forEach { h ->
            Box(Modifier.width(3.dp).height(h.dp).clip(RoundedCornerShape(1.dp)).background(Palette.Good))
        }
    }
}

@Composable
private fun ToolDivider() = Box(Modifier.width(1.dp).height(30.dp).background(Color(0x5545649E)))

@Composable
private fun ToolButton(label: String, icon: Int, onClick: () -> Unit) {
    Column(
        Modifier.clip(RoundedCornerShape(10.dp)).clickable(onClick = onClick).padding(horizontal = 16.dp, vertical = 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(painterResource(icon), contentDescription = label, tint = Color.White, modifier = Modifier.size(22.dp))
        Spacer(Modifier.height(2.dp))
        Text(label, color = Color.White, fontSize = 12.sp)
    }
}

/** Types into the PC as you type here: added characters are sent, deleted ones become Backspace. */
@Composable
private fun KeyboardPanel(send: (String) -> Unit, modifier: Modifier) {
    var value by remember { mutableStateOf("") }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    fun typed(next: String) {
        val common = value.commonPrefixWith(next).length
        repeat(value.length - common) { send(InputMessages.key(VK.BACK)) }
        val added = next.substring(common)
        if (added.isNotEmpty()) send(InputMessages.text(added))
        value = next
    }
    Column(
        modifier.fillMaxWidth().padding(12.dp).glass(20).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            KeyBox(Modifier.size(width = 64.dp, height = 52.dp), onClick = { focus.requestFocus() }) {
                Icon(painterResource(R.drawable.ic_keyboard), contentDescription = null, tint = Color.White, modifier = Modifier.size(26.dp))
            }
            val shape = RoundedCornerShape(14.dp)
            Row(
                Modifier.weight(1f).height(52.dp).clip(shape).background(Color(0x66040B24)).border(1.dp, Color(0xFF2A4A8A), shape)
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                BasicTextField(
                    value = value,
                    onValueChange = { typed(it) },
                    singleLine = true,
                    textStyle = TextStyle(color = Palette.Text, fontSize = 17.sp, fontFamily = Plex),
                    cursorBrush = SolidColor(Palette.Glow),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                    keyboardActions = KeyboardActions(onSend = { send(InputMessages.key(VK.ENTER)); value = "" }),
                    modifier = Modifier.weight(1f).focusRequester(focus),
                    decorationBox = { inner ->
                        Box {
                            if (value.isEmpty()) Text("اكتب هنا ليظهر على الكمبيوتر", color = Palette.TextMuted, fontSize = 17.sp)
                            inner()
                        }
                    },
                )
                Icon(painterResource(R.drawable.ic_close), contentDescription = "مسح", tint = Palette.Text,
                    modifier = Modifier.size(24.dp).clip(RoundedCornerShape(6.dp)).clickable { value = "" })
            }
        }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            // Right-to-left row: the arrows sit on the right, as on the design.
            ArrowKey(R.drawable.ic_arrow_right) { send(InputMessages.key(VK.RIGHT)) }
            ArrowKey(R.drawable.ic_arrow_up) { send(InputMessages.key(VK.UP)) }
            ArrowKey(R.drawable.ic_arrow_down) { send(InputMessages.key(VK.DOWN)) }
            ArrowKey(R.drawable.ic_arrow_left) { send(InputMessages.key(VK.LEFT)) }
            Spacer(Modifier.weight(1f))
            TextKey("Esc") { send(InputMessages.key(VK.ESC)) }
            TextKey("Tab") { send(InputMessages.key(VK.TAB)) }
            KeyBox(Modifier.size(width = 104.dp, height = 52.dp), onClick = { send(InputMessages.key(VK.BACK)); value = value.dropLast(1) }) {
                Icon(painterResource(R.drawable.ic_backspace), contentDescription = "Backspace", tint = Color.White, modifier = Modifier.size(26.dp))
            }
            KeyBox(Modifier.size(width = 148.dp, height = 52.dp), onClick = { send(InputMessages.key(VK.ENTER)); value = "" }) {
                CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("Enter", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
                        Spacer(Modifier.width(6.dp))
                        Icon(painterResource(R.drawable.ic_enter), contentDescription = null, tint = Color.White, modifier = Modifier.size(20.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun KeyBox(modifier: Modifier, onClick: () -> Unit, content: @Composable BoxScope.() -> Unit) {
    val shape = RoundedCornerShape(12.dp)
    Box(
        modifier.clip(shape).background(Brush.verticalGradient(listOf(Color(0xFF15306F), Color(0xFF0C1F52))))
            .border(1.dp, Color(0xFF2E4F92), shape).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
        content = content,
    )
}

@Composable
private fun TextKey(label: String, onClick: () -> Unit) =
    KeyBox(Modifier.size(width = 112.dp, height = 52.dp), onClick) {
        Text(label, color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.SemiBold)
    }

@Composable
private fun ArrowKey(icon: Int, onClick: () -> Unit) =
    KeyBox(Modifier.size(width = 58.dp, height = 52.dp), onClick) {
        Icon(painterResource(icon), contentDescription = null, tint = Color.White, modifier = Modifier.size(24.dp))
    }

@Composable
private fun ShortcutsPanel(send: (String) -> Unit, modifier: Modifier) {
    val items = listOf(
        "نسخ" to intArrayOf(VK.CTRL, VK.letter('C')),
        "لصق" to intArrayOf(VK.CTRL, VK.letter('V')),
        "قص" to intArrayOf(VK.CTRL, VK.letter('X')),
        "تراجع" to intArrayOf(VK.CTRL, VK.letter('Z')),
        "تحديد الكل" to intArrayOf(VK.CTRL, VK.letter('A')),
        "حفظ" to intArrayOf(VK.CTRL, VK.letter('S')),
        "تبديل النوافذ" to intArrayOf(VK.ALT, VK.TAB),
        "سطح المكتب" to intArrayOf(VK.WIN, VK.letter('D')),
        "قائمة ابدأ" to intArrayOf(VK.WIN),
        "لقطة شاشة" to intArrayOf(VK.WIN, VK.SHIFT, VK.letter('S')),
        "إغلاق النافذة" to intArrayOf(VK.ALT, 0x73),
        "مدير المهام" to intArrayOf(VK.CTRL, VK.SHIFT, 0x1B),
    )
    Column(
        modifier.fillMaxWidth().padding(12.dp).glass(20).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        items.chunked(6).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                row.forEach { (label, keys) ->
                    KeyBox(Modifier.weight(1f).height(50.dp), onClick = {
                        send(if (keys.size == 1) InputMessages.key(keys[0]) else InputMessages.combo(*keys))
                    }) { Text(label, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Medium, textAlign = TextAlign.Center) }
                }
            }
        }
    }
}

/**
 * Turn a finger around the dial: each notch sends one step.
 * Tap the centre to change what it does: brush size ([ ]), zoom, scroll, undo/redo.
 */
@Composable
private fun Dial(send: (String) -> Unit, modifier: Modifier) {
    val modes = listOf("الفرشاة", "تكبير", "تمرير", "تراجع")
    var mode by remember { mutableIntStateOf(0) }
    var angle by remember { mutableFloatStateOf(0f) }
    fun step(dir: Int) {
        when (mode) {
            0 -> send(InputMessages.key(if (dir > 0) 0xDD else 0xDB))          // ] bigger, [ smaller
            1 -> send(InputMessages.zoom(0.5f, 0.5f, 120 * dir))
            2 -> send(InputMessages.scroll(0.5f, 0.5f, -120 * dir))
            3 -> send(if (dir > 0) InputMessages.combo(VK.CTRL, VK.letter('Y')) else InputMessages.combo(VK.CTRL, VK.letter('Z')))
        }
    }
    Box(
        modifier.size(190.dp).clip(CircleShape)
            .background(Brush.radialGradient(listOf(Color(0xEE15306F), Color(0xEE0A1A4A))))
            .border(2.dp, Color(0xFF2E4F92), CircleShape)
            .pointerInput(Unit) {
                var last = Float.NaN
                var acc = 0f
                awaitEachGesture {
                    val down = awaitFirstDown()
                    val c = androidx.compose.ui.geometry.Offset(size.width / 2f, size.height / 2f)
                    val startInCentre = (down.position - c).getDistance() < size.width * 0.22f
                    last = Float.NaN; acc = 0f
                    var moved = false
                    while (true) {
                        val ev = awaitPointerEvent()
                        val ch = ev.changes.firstOrNull() ?: break
                        if (!ch.pressed) break
                        val v = ch.position - c
                        val a = Math.toDegrees(kotlin.math.atan2(v.y.toDouble(), v.x.toDouble())).toFloat()
                        if (!last.isNaN()) {
                            var d = a - last
                            if (d > 180) d -= 360
                            if (d < -180) d += 360
                            acc += d
                            angle += d
                            if (kotlin.math.abs(acc) > 3) moved = true
                            while (kotlin.math.abs(acc) >= 18f) {
                                val dir = if (acc > 0) 1 else -1
                                step(dir)
                                acc -= dir * 18f
                            }
                        }
                        last = a
                        ch.consume()
                    }
                    if (startInCentre && !moved) mode = (mode + 1) % modes.size
                }
            },
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.fillMaxSize()) {
            val r = size.minDimension / 2
            for (i in 0 until 20) {
                val t = Math.toRadians((i * 18 + angle).toDouble())
                val o = androidx.compose.ui.geometry.Offset(center.x + (r * 0.82f) * kotlin.math.cos(t).toFloat(), center.y + (r * 0.82f) * kotlin.math.sin(t).toFloat())
                drawCircle(if (i == 0) Color(0xFF59B8FF) else Color(0x6659B8FF), radius = if (i == 0) 7f else 4f, center = o)
            }
        }
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Text(modes[mode], color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.Bold)
            Text("اضغط للتغيير", color = Palette.TextMuted, fontSize = 11.sp)
        }
    }
}

/** Keyboard panel with three faces: typing, number pad, full PC keyboard. */
@Composable
private fun KeyboardTabs(send: (String) -> Unit, modifier: Modifier) {
    var tab by remember { mutableIntStateOf(0) }
    Column(modifier.fillMaxWidth()) {
        Row(Modifier.padding(start = 24.dp, end = 24.dp).glass(14).padding(4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            listOf("كتابة", "أرقام", "لوحة كاملة").forEachIndexed { i, label ->
                val on = i == tab
                Box(
                    Modifier.clip(RoundedCornerShape(10.dp))
                        .then(if (on) Modifier.background(Brush.verticalGradient(listOf(Color(0xFF3AA2FF), Color(0xFF1668E8)))) else Modifier)
                        .clickable { tab = i }.padding(horizontal = 18.dp, vertical = 8.dp),
                ) { Text(label, color = Color.White, fontSize = 14.sp, fontWeight = if (on) FontWeight.Bold else FontWeight.Normal) }
            }
        }
        when (tab) {
            0 -> KeyboardPanel(send, Modifier)
            1 -> NumberPad(send)
            else -> FullKeyboard(send)
        }
    }
}

@Composable
private fun NumberPad(send: (String) -> Unit) {
    val rows = listOf(
        listOf("7" to 0x67, "8" to 0x68, "9" to 0x69, "÷" to 0x6F, "⌫" to VK.BACK),
        listOf("4" to 0x64, "5" to 0x65, "6" to 0x66, "×" to 0x6A, "Tab" to VK.TAB),
        listOf("1" to 0x61, "2" to 0x62, "3" to 0x63, "−" to 0x6D, "Esc" to VK.ESC),
        listOf("0" to 0x60, "00" to -1, "." to 0x6E, "+" to 0x6B, "Enter" to VK.ENTER),
    )
    Column(Modifier.fillMaxWidth().padding(12.dp).glass(20).padding(14.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                rows.forEach { row ->
                    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        row.forEach { (label, vk) ->
                            KeyBox(Modifier.weight(1f).height(54.dp), onClick = {
                                if (vk == -1) { send(InputMessages.key(0x60)); send(InputMessages.key(0x60)) } else send(InputMessages.key(vk))
                            }) { Text(label, color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.SemiBold) }
                        }
                    }
                }
            }
        }
    }
}

/** A PC keyboard: Ctrl / Shift / Alt / Win stay down for the next key (tap again to release). */
@Composable
private fun FullKeyboard(send: (String) -> Unit) {
    val held = remember { mutableStateListOf<Int>() }
    fun press(vk: Int) {
        if (vk in listOf(VK.CTRL, VK.SHIFT, VK.ALT, VK.WIN)) {
            if (vk in held) { held.remove(vk); send(InputMessages.keyUp(vk)) } else { held.add(vk); send(InputMessages.keyDown(vk)) }
            return
        }
        send(InputMessages.key(vk))
        held.toList().forEach { send(InputMessages.keyUp(it)) }
        held.clear()
    }
    DisposableEffect(Unit) { onDispose { held.toList().forEach { send(InputMessages.keyUp(it)) } } }
    fun letters(s: String) = s.map { it.toString().uppercase() to it.uppercaseChar().code }
    val f = (1..12).map { "F$it" to (0x6F + it) }
    val rows = listOf(
        listOf("Esc" to VK.ESC) + f + listOf("Del" to VK.DELETE),
        listOf("`" to 0xC0) + "1234567890".map { it.toString() to it.code } + listOf("-" to 0xBD, "=" to 0xBB, "⌫" to VK.BACK),
        listOf("Tab" to VK.TAB) + letters("qwertyuiop") + listOf("[" to 0xDB, "]" to 0xDD, "\\" to 0xDC),
        listOf("Caps" to 0x14) + letters("asdfghjkl") + listOf(";" to 0xBA, "'" to 0xDE, "Enter" to VK.ENTER),
        listOf("Shift" to VK.SHIFT) + letters("zxcvbnm") + listOf("," to 0xBC, "." to 0xBE, "/" to 0xBF, "↑" to VK.UP),
        listOf("Ctrl" to VK.CTRL, "Win" to VK.WIN, "Alt" to VK.ALT, "مسافة" to VK.SPACE, "Home" to 0x24, "End" to 0x23, "←" to VK.LEFT, "↓" to VK.DOWN, "→" to VK.RIGHT),
    )
    Column(Modifier.fillMaxWidth().padding(12.dp).glass(20).padding(10.dp)) {
        CompositionLocalProvider(LocalLayoutDirection provides LayoutDirection.Ltr) {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                rows.forEach { row ->
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        row.forEach { (label, vk) ->
                            val wide = label.length > 2 || vk == VK.SPACE
                            val on = vk in held
                            val shape = RoundedCornerShape(9.dp)
                            Box(
                                Modifier.weight(if (vk == VK.SPACE) 4f else if (wide) 1.5f else 1f).height(46.dp).clip(shape)
                                    .background(if (on) Brush.verticalGradient(listOf(Color(0xFF3AA2FF), Color(0xFF1668E8)))
                                                else Brush.verticalGradient(listOf(Color(0xFF15306F), Color(0xFF0C1F52))))
                                    .border(1.dp, Color(0xFF2E4F92), shape).clickable { press(vk) },
                                contentAlignment = Alignment.Center,
                            ) { Text(label, color = Color.White, fontSize = if (wide) 13.sp else 16.sp, maxLines = 1) }
                        }
                    }
                }
            }
        }
    }
}
