package com.screenlink.app

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONObject
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import java.net.DatagramPacket
import java.net.DatagramSocket
import kotlin.concurrent.thread

/**
 * The one control connection to the PC, shared by every screen of the app:
 * status, PC name, latency, PC load, PIN check, shared clipboard and Wake-on-LAN.
 */
object Link {
    var status by mutableStateOf(TcpClient.STATUS_DISCONNECTED); private set
    var hostName by mutableStateOf("الكمبيوتر"); private set
    var hostOs by mutableStateOf("Windows"); private set
    var latencyMs by mutableStateOf<Long?>(null); private set
    var cpu by mutableStateOf<Int?>(null); private set
    var ram by mutableStateOf<Int?>(null); private set
    var ramText by mutableStateOf(""); private set
    var needPin by mutableStateOf(false); private set
    var pinError by mutableStateOf(false); private set
    /** Seconds to wait after too many wrong PINs (0 = may try now). */
    var pinWait by mutableIntStateOf(0); private set
    var authed by mutableStateOf(false); private set
    var lastClip by mutableStateOf(""); private set
    /** Program in front on the PC, e.g. "photoshop.exe". */
    var foregroundExe by mutableStateOf(""); private set
    /** Volume mixer: master (v, muted) and programs (pid, name, v, muted). */
    var master by mutableStateOf<Pair<Float, Boolean>?>(null); private set
    var appVolumes by mutableStateOf<List<AppVolume>>(emptyList()); private set
    /** Colour picked under the PC's mouse pointer, "#RRGGBB". */
    var pickedColor by mutableStateOf(""); private set
    /** PC windows for "show one window only": (id, title). */
    var windows by mutableStateOf<List<Pair<Long, String>>>(emptyList()); private set
    var actionDone by mutableIntStateOf(0); private set
    /** Goes up when the PC stops the stream on purpose (its "stop" menu, or it is closing). */
    var pcStoppedStream by mutableIntStateOf(0); private set
    /** Set by the quick-settings tile: start showing as soon as the PC answers. */
    var startWhenReady by mutableStateOf(false)
    /** True while the stream screen is open (no automatic second start). */
    var streaming by mutableStateOf(false)

    val connected get() = status == TcpClient.STATUS_CONNECTED && authed

    private lateinit var app: Context
    private lateinit var client: TcpClient
    private val main = Handler(Looper.getMainLooper())
    private var host = ""
    private var pingSentAt = 0L
    private var started = false
    private var settingClip = false
    /** Which PC this connection is: its network card, or its address if the card is unknown. */
    private var pcId = ""

    fun init(context: Context) {
        if (started) return
        started = true
        app = context.applicationContext
        client = TcpClient(hello = {
            val p = Prefs.load(app)
            Messages.hello(p.deviceId, "${Build.MANUFACTURER} ${Build.MODEL}")
        }) { newStatus, reply -> onUpdate(newStatus, reply) }
        val clipboard = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.addPrimaryClipChangedListener {
            if (settingClip) return@addPrimaryClipChangedListener
            val text = runCatching { clipboard.primaryClip?.getItemAt(0)?.coerceToText(app)?.toString() }.getOrNull()
            if (!text.isNullOrEmpty() && text != lastClip && text.length <= 1500 && connected) {
                lastClip = text
                client.send(Messages.clip(text))
            }
        }
        tick()
    }

    private fun tick() {
        main.postDelayed({
            if (status == TcpClient.STATUS_CONNECTED) {
                pingSentAt = System.currentTimeMillis()
                client.send(Messages.ping())
            } else if (status != TcpClient.STATUS_CONNECTING && host.isNotBlank()) {
                client.connect(host)  // cable plugged in later, PC started later, Wi-Fi came back
            }
            tick()
        }, 2500)
    }

    private fun onUpdate(newStatus: String, reply: String?) {
        status = newStatus
        if (newStatus != TcpClient.STATUS_CONNECTED) {
            latencyMs = null; cpu = null; ram = null; authed = false
        }
        val j = reply?.let { runCatching { JSONObject(it) }.getOrNull() } ?: return
        when (j.optString("type")) {
            "WELCOME" -> {
                hostName = j.optString("host_name", "الكمبيوتر").ifBlank { "الكمبيوتر" }
                hostOs = j.optString("os", "Windows")
            }
            "INFO" -> {
                val mac = j.optString("mac")
                if (mac.length == 17) {
                    val p = Prefs.load(app)
                    if (p.mac != mac) p.copy(mac = mac).save(app)
                }
                pcId = if (mac.length == 17) mac.uppercase() else "host:$host"
                authed = j.optString("auth") == "ok"
                pinError = false
                pinWait = 0
                val nonce = j.optString("nonce")
                val key = Keys.get(app, pcId)
                if (!authed && key != null && nonce.isNotEmpty()) {
                    // Paired before: sign this connection's random value instead of asking for the PIN.
                    val p = Prefs.load(app)
                    client.send(Messages.authProof(Keys.proof(key, nonce, p.deviceId), p.deviceId, deviceName()))
                    needPin = false
                } else {
                    needPin = !authed
                }
            }
            "AUTH_OK" -> {
                j.optString("key").takeIf { it.length == 64 && pcId.isNotEmpty() }?.let { Keys.put(app, pcId, it) }
                authed = true; needPin = false; pinError = false; pinWait = 0
            }
            "AUTH_FAIL" -> {
                if (j.optBoolean("reset")) {
                    // The PC no longer knows this device (devices forgotten, or paired with an older version).
                    Keys.remove(app, pcId)
                    pinError = false
                } else {
                    pinError = true
                }
                pinWait = j.optInt("wait", 0)
                needPin = true
            }
            "PONG" -> if (pingSentAt > 0) latencyMs = System.currentTimeMillis() - pingSentAt
            "STATS" -> {
                cpu = j.optInt("cpu")
                ram = j.optInt("ram")
                ramText = "%.1f / %.0f GB".format(j.optDouble("ram_used_gb"), j.optDouble("ram_total_gb"))
            }
            "CLIP" -> {
                val text = j.optString("text")
                if (text.isNotEmpty() && text != lastClip) {
                    lastClip = text
                    settingClip = true
                    runCatching {
                        (app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                            .setPrimaryClip(ClipData.newPlainText("امتداد", text))
                    }
                    main.postDelayed({ settingClip = false }, 300)
                }
            }
            "ACTION_DONE" -> actionDone++
            "STREAM_STOPPED" -> pcStoppedStream++
            "COLOR" -> pickedColor = j.optString("hex")
            "APP" -> foregroundExe = j.optString("exe")
            "VOLUMES" -> {
                j.optJSONObject("master")?.let { master = it.optDouble("v").toFloat() to it.optBoolean("m") }
                val arr = j.optJSONArray("apps")
                appVolumes = (0 until (arr?.length() ?: 0)).mapNotNull { i ->
                    arr?.optJSONObject(i)?.let { AppVolume(it.optLong("pid"), it.optString("name"), it.optDouble("v").toFloat(), it.optBoolean("m")) }
                }
            }
            "WINDOWS" -> {
                val arr = j.optJSONArray("list")
                windows = (0 until (arr?.length() ?: 0)).mapNotNull { i ->
                    arr?.optJSONObject(i)?.let { it.optLong("id") to it.optString("title") }
                }
            }
            "FILE" -> if (host.isNotBlank()) FileTransfer.receive(app, host, j.optString("id"))
        }
    }

    val currentHost: String get() = host

    fun connect(newHost: String) {
        host = newHost
        client.connect(newHost)
    }

    fun disconnect() {
        host = ""
        client.disconnect()
    }

    fun send(message: String) = client.send(message)

    fun sendPin(pin: String) {
        val p = Prefs.load(app)
        client.send(Messages.auth(pin.trim(), p.deviceId, deviceName()))
    }

    private fun deviceName() = "${Build.MANUFACTURER} ${Build.MODEL}"

    fun dismissPin() { needPin = false }

    fun clearColor() { pickedColor = "" }

    fun requestVolumes() = client.send(JSONObject().put("type", "VOLUMES").toString())

    /** pid 0 = the PC's speakers. */
    fun setVolume(pid: Long, v: Float? = null, mute: Boolean? = null) {
        val m = JSONObject().put("type", "VOLUME").put("pid", pid)
        if (v != null) m.put("v", v.toDouble())
        if (mute != null) m.put("m", mute)
        client.send(m.toString())
        if (pid == 0L) master = (v ?: master?.first ?: 0f) to (mute ?: master?.second ?: false)
        else appVolumes = appVolumes.map { if (it.pid == pid) it.copy(v = v ?: it.v, muted = mute ?: it.muted) else it }
    }

    /** Pointer input without a picture (air mouse, touchpad on the control page). */
    fun input(json: String) = client.send(JSONObject().put("type", "INPUT").put("e", json).toString())

    fun requestWindows() { windows = emptyList(); client.send(org.json.JSONObject().put("type", "WINDOWS").toString()) }

    /** Sends this device's clipboard text to the PC (the app must be in front to read it). */
    fun sendClipboard(): Boolean {
        val clipboard = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val text = runCatching { clipboard.primaryClip?.getItemAt(0)?.coerceToText(app)?.toString() }.getOrNull()
        if (text.isNullOrEmpty() || !connected) return false
        lastClip = text.take(1500)
        client.send(Messages.clip(lastClip))
        return true
    }

    /** Wake-on-LAN "magic packet" to the PC's network card (same Wi-Fi; must be enabled in the PC's BIOS). */
    fun wake(mac: String): Boolean {
        val bytes = mac.split(':', '-').mapNotNull { it.toIntOrNull(16)?.toByte() }
        if (bytes.size != 6) return false
        val packet = ByteArray(6 + 16 * 6)
        for (i in 0 until 6) packet[i] = 0xFF.toByte()
        for (i in 0 until 16) for (k in 0 until 6) packet[6 + i * 6 + k] = bytes[k]
        thread(name = "imtidad-wol") {
            runCatching {
                DatagramSocket().use { s ->
                    s.broadcast = true
                    repeat(3) {
                        for (target in Discovery.broadcastAddresses()) {
                            for (port in intArrayOf(9, 7)) runCatching { s.send(DatagramPacket(packet, packet.size, target, port)) }
                        }
                        Thread.sleep(200)
                    }
                }
            }
        }
        return true
    }
}

/** Keys from pairing with a PIN, one per PC (app-private storage). */
private object Keys {
    private fun prefs(context: Context) = context.getSharedPreferences("imtidad_keys", Context.MODE_PRIVATE)
    fun get(context: Context, pc: String): String? = prefs(context).getString(pc, null)?.takeIf { it.length == 64 }
    fun put(context: Context, pc: String, key: String) { prefs(context).edit().putString(pc, key).apply() }
    fun remove(context: Context, pc: String) { if (pc.isNotEmpty()) prefs(context).edit().remove(pc).apply() }

    fun proof(keyHex: String, nonce: String, id: String): String {
        val key = ByteArray(keyHex.length / 2) { i -> keyHex.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal((nonce + id).toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
    }
}

data class AppVolume(val pid: Long, val name: String, val v: Float, val muted: Boolean)
