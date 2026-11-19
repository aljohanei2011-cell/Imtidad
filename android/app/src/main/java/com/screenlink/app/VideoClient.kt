package com.screenlink.app

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.Surface
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/** Stream settings sent to the PC as the first line on the video connection. */
data class StreamConfig(
    val fps: Int = 30,
    val bitrate: Int = 8_000_000,
    val maxWidth: Int = 2560,
    val extend: Boolean = false,   // stream a virtual second screen instead of the main one
    val width: Int = 0,            // virtual screen size requested in extend mode
    val height: Int = 0,
    val hevc: Boolean = false,     // ask for H.265 (the PC answers SLV2 with the codec it really uses)
    val adaptive: Boolean = true,  // lower / raise the bitrate when the network slows down / recovers
    val window: Long = 0,          // one PC window only (its id from the PC's window list)
    val pad: Boolean = false,      // drawing pad: no picture, the device is a pen tablet for the PC
) {
    fun toJson(): String = JSONObject().put("t", "cfg").put("fps", fps).put("bitrate", bitrate)
        .put("max_width", maxWidth).put("display", if (pad) "pad" else if (extend) "virtual" else "primary")
        .put("width", width).put("height", height).put("codec", if (hevc) "hevc" else "h264").put("window", window).toString()

    companion object {
        /** True if this device can decode H.265 in hardware. */
        fun deviceDecodesHevc(): Boolean = runCatching {
            android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS).codecInfos.any { info ->
                !info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_HEVC, true) } &&
                    (Build.VERSION.SDK_INT < 29 || info.isHardwareAccelerated)
            }
        }.getOrDefault(false)
    }
}

/**
 * Receives the H.264 stream from the PC (see windows/src/stream.hpp for the wire format),
 * decodes it with MediaCodec straight onto a Surface, and sends input events back.
 */
class VideoClient(
    private val host: String,
    private val config: StreamConfig,
    private val listener: Listener,
    private val port: Int = VIDEO_PORT,
) {
    interface Listener {
        fun onVideoSize(width: Int, height: Int)
        fun onStats(fps: Int, kbps: Int)
        fun onCodec(hevc: Boolean) {}
        fun onQuality(bitrate: Int, lagMs: Int) {}
        fun onError(message: String)
        fun onClosed()
    }

    private val main = Handler(Looper.getMainLooper())
    private val sender = Executors.newSingleThreadExecutor()
    private val outLock = Any()
    @Volatile private var running = false
    /** Set by stop(): the screen closed this stream itself, so nobody needs to hear that it ended. */
    @Volatile private var stoppedByUser = false
    @Volatile private var socket: Socket? = null
    @Volatile private var out: BufferedOutputStream? = null
    @Volatile private var codec: MediaCodec? = null
    @Volatile private var recorder: Recorder? = null
    @Volatile private var pendingRecorder: Recorder? = null
    private var streamW = 0
    private var streamH = 0
    private var streamHevc = false

    /** Starts recording what arrives into Movies/Imtidad (from the next keyframe). */
    fun startRecording(context: android.content.Context): Boolean {
        if (streamW == 0) return false
        pendingRecorder = Recorder(context.applicationContext, streamW, streamH, streamHevc)
        sendInput(JSONObject().put("t", "kf").toString())  // ask the PC for a keyframe now
        return true
    }

    /** Stops recording; returns the saved file name or null. */
    fun stopRecording(): String? {
        pendingRecorder = null
        val r = recorder ?: return null
        recorder = null
        return r.stop()
    }

    fun start(surface: Surface) {
        if (running) return
        running = true
        thread(name = "screenlink-video") { receive(surface) }
    }

    fun stop() {
        stoppedByUser = true
        running = false
        runCatching { socket?.close() }
        sender.shutdownNow()
    }

    fun sendInput(json: String) {
        if (sender.isShutdown) return
        sender.execute {
            try {
                synchronized(outLock) {
                    val o = out ?: return@execute
                    o.write((json + "\n").toByteArray(Charsets.UTF_8))
                    o.flush()
                }
            } catch (_: IOException) {
            }
        }
    }

    private fun receive(surface: Surface) {
        var error: String? = null
        var outputThread: Thread? = null
        try {
            val s = Socket()
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), 4000)
            socket = s
            out = BufferedOutputStream(s.getOutputStream())
            sendInput(config.toJson())
            val input = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 16))

            val magic = ByteArray(4)
            input.readFully(magic)
            var hevc = false
            val version = String(magic, Charsets.US_ASCII)
            when (version) {
                "SLV1", "SLV2" -> {}
                "SLER" -> {
                    val n = input.readInt().coerceIn(0, 4096)
                    val msg = ByteArray(n)
                    input.readFully(msg)
                    throw IOException("الكمبيوتر: " + String(msg, Charsets.UTF_8))
                }
                else -> throw IOException("استجابة غير متوقعة من الكمبيوتر")
            }
            val width = input.readInt()
            val height = input.readInt()
            input.readInt() // fps
            if (version == "SLV2") hevc = input.readInt() == 1
            streamW = width; streamH = height; streamHevc = hevc
            main.post { listener.onVideoSize(width, height); listener.onCodec(hevc) }

            // Adaptive quality: the PC stamps each frame with its capture time. The smallest
            // (arrival - capture) gap seen is the normal delay; anything above it is queueing in
            // the network. Rising queueing → lower the bitrate; steady and low → raise it again.
            var bitrate = config.bitrate
            val minBitrate = 1_500_000
            var baseOffset = Long.MAX_VALUE
            var lagSum = 0L
            var lagCount = 0
            var calmWindows = 0
            var windowStart = System.nanoTime()

            var buf = ByteArray(1 shl 20)
            var statFrames = 0
            var statBytes = 0L
            var statStart = System.nanoTime()
            while (running) {
                val len = input.readInt()
                val ptsUs = input.readLong()
                if (len <= 0 || len > (32 shl 20)) throw IOException("حجم إطار غير صالح: $len")
                if (len > buf.size) buf = ByteArray(len)
                input.readFully(buf, 0, len)

                pendingRecorder?.let { recorder = it; pendingRecorder = null }
                recorder?.let { r -> if (!r.write(buf, len, ptsUs)) recorder = null }

                if (config.adaptive) {
                    val offset = System.nanoTime() / 1000 - ptsUs
                    if (offset < baseOffset) baseOffset = offset
                    lagSum += (offset - baseOffset) / 1000
                    lagCount++
                    val nowNs = System.nanoTime()
                    if (nowNs - windowStart >= 2_000_000_000L && lagCount > 0) {
                        val lag = (lagSum / lagCount).toInt()
                        val next = when {
                            lag > 150 -> { calmWindows = 0; (bitrate * 0.65).toInt().coerceAtLeast(minBitrate) }
                            lag > 80 -> { calmWindows = 0; (bitrate * 0.85).toInt().coerceAtLeast(minBitrate) }
                            lag < 35 -> { calmWindows++; if (calmWindows >= 3) (bitrate * 1.15).toInt().coerceAtMost(config.bitrate) else bitrate }
                            else -> { calmWindows = 0; bitrate }
                        }
                        if (next != bitrate) {
                            bitrate = next
                            if (calmWindows >= 3) calmWindows = 0
                            sendInput(JSONObject().put("t", "br").put("bitrate", bitrate).toString())
                            // The queue drains faster when the old estimate is forgotten.
                            baseOffset = Long.MAX_VALUE
                        }
                        val b = bitrate
                        main.post { listener.onQuality(b, lag) }
                        lagSum = 0; lagCount = 0; windowStart = nowNs
                    }
                }

                var c = codec
                if (c == null) {
                    val csd = (if (hevc) Hevc.findParameterSets(buf, len) else H264.findParameterSets(buf, len)?.let { listOf(it.first, it.second) })
                        ?: continue  // wait for a keyframe
                    c = createDecoder(width, height, csd, hevc, surface)
                    codec = c
                    outputThread = thread(name = "screenlink-render") { render(c) }
                }
                val index = c.dequeueInputBuffer(50_000)
                if (index >= 0) {
                    val ib = c.getInputBuffer(index) ?: continue
                    ib.clear()
                    ib.put(buf, 0, len)
                    c.queueInputBuffer(index, 0, len, ptsUs, 0)
                }

                statFrames++
                statBytes += len
                val now = System.nanoTime()
                if (now - statStart >= 1_000_000_000L) {
                    val secs = (now - statStart) / 1e9
                    val fps = (statFrames / secs).toInt()
                    val kbps = (statBytes * 8 / 1000 / secs).toInt()
                    main.post { listener.onStats(fps, kbps) }
                    statFrames = 0
                    statBytes = 0
                    statStart = now
                }
            }
        } catch (e: Exception) {
            if (running) error = e.message ?: e.javaClass.simpleName
        } finally {
            running = false
            runCatching { socket?.close() }
            outputThread?.interrupt()
            runCatching { outputThread?.join(500) }
            recorder?.stop()
            recorder = null
            codec?.let { runCatching { it.stop() }; runCatching { it.release() } }
            codec = null
            val err = error
            main.post {
                if (stoppedByUser) return@post
                if (err != null) listener.onError(err)
                listener.onClosed()
            }
        }
    }

    private fun render(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        try {
            while (running) {
                val index = c.dequeueOutputBuffer(info, 50_000)
                if (index >= 0) c.releaseOutputBuffer(index, true)  // show immediately
            }
        } catch (_: Exception) {
            // codec stopped
        }
    }

    private fun createDecoder(w: Int, h: Int, csd: List<ByteArray>, hevc: Boolean, surface: Surface): MediaCodec {
        val mime = if (hevc) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        fun format(lowLatency: Boolean) = MediaFormat.createVideoFormat(mime, w, h).apply {
            if (hevc) {
                // H.265: VPS + SPS + PPS together in csd-0.
                val all = java.io.ByteArrayOutputStream()
                csd.forEach { all.write(it) }
                setByteBuffer("csd-0", ByteBuffer.wrap(all.toByteArray()))
            } else {
                setByteBuffer("csd-0", ByteBuffer.wrap(csd[0]))
                setByteBuffer("csd-1", ByteBuffer.wrap(csd[1]))
            }
            setInteger(MediaFormat.KEY_PRIORITY, 0)  // realtime
            if (lowLatency && Build.VERSION.SDK_INT >= 30) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        val codec = MediaCodec.createDecoderByType(mime)
        try {
            codec.configure(format(true), surface, null, 0)
        } catch (e: Exception) {
            // Some decoders reject the low-latency key: retry without it.
            codec.reset()
            codec.configure(format(false), surface, null, 0)
        }
        codec.start()
        return codec
    }

    companion object {
        const val VIDEO_PORT = 48766
    }
}

/** Minimal H.264 Annex-B helpers. */
object H264 {
    /** Returns (SPS, PPS) each with a 4-byte start code, or null if the access unit has none. */
    fun findParameterSets(data: ByteArray, length: Int): Pair<ByteArray, ByteArray>? {
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        for ((start, end) in nalUnits(data, length)) {
            when (data[start].toInt() and 0x1F) {
                7 -> sps = withStartCode(data, start, end)
                8 -> pps = withStartCode(data, start, end)
            }
        }
        return if (sps != null && pps != null) sps to pps else null
    }

    /** NAL payload ranges [start, end) excluding start codes. */
    fun nalUnits(data: ByteArray, length: Int): List<Pair<Int, Int>> {
        val starts = ArrayList<Pair<Int, Int>>() // (startCodeIndex, payloadStart)
        var i = 0
        while (i + 3 <= length) {
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0) {
                if (data[i + 2].toInt() == 1) {
                    starts.add(i to i + 3); i += 3; continue
                }
                if (i + 4 <= length && data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1) {
                    starts.add(i to i + 4); i += 4; continue
                }
            }
            i++
        }
        val result = ArrayList<Pair<Int, Int>>()
        for (k in starts.indices) {
            val payload = starts[k].second
            val end = if (k + 1 < starts.size) starts[k + 1].first else length
            if (payload < end) result.add(payload to end)
        }
        return result
    }

    fun withStartCode(data: ByteArray, start: Int, end: Int): ByteArray {
        val out = ByteArray(4 + end - start)
        out[3] = 1
        System.arraycopy(data, start, out, 4, end - start)
        return out
    }
}

/** Minimal H.265 Annex-B helper. */
object Hevc {
    /** Returns [VPS, SPS, PPS] each with a start code, or null if the access unit lacks them. */
    fun findParameterSets(data: ByteArray, length: Int): List<ByteArray>? {
        var vps: ByteArray? = null
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        for ((start, end) in H264.nalUnits(data, length)) {
            when ((data[start].toInt() shr 1) and 0x3F) {
                32 -> vps = H264.withStartCode(data, start, end)
                33 -> sps = H264.withStartCode(data, start, end)
                34 -> pps = H264.withStartCode(data, start, end)
            }
        }
        return if (vps != null && sps != null && pps != null) listOf(vps!!, sps!!, pps!!) else null
    }
}
