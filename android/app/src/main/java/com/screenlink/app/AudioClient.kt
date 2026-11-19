package com.screenlink.app

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import java.io.BufferedInputStream
import java.io.DataInputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.concurrent.thread

/**
 * Plays the computer's sound (16-bit PCM from windows/src/audio.cpp) with low latency.
 * If data piles up (more than ~120 ms waiting), old chunks are skipped so sound stays in sync.
 */
class AudioClient(private val host: String, private val port: Int = AUDIO_PORT) {
    @Volatile private var running = false
    @Volatile private var socket: Socket? = null

    fun start() {
        if (running) return
        running = true
        thread(name = "imtidad-audio") { loop() }
    }

    fun stop() {
        running = false
        runCatching { socket?.close() }
    }

    private fun loop() {
        var track: AudioTrack? = null
        try {
            val s = Socket()
            s.tcpNoDelay = true
            s.connect(InetSocketAddress(host, port), 4000)
            socket = s
            val input = DataInputStream(BufferedInputStream(s.getInputStream(), 1 shl 15))
            val magic = ByteArray(4)
            input.readFully(magic)
            if (String(magic, Charsets.US_ASCII) != "SLA1") throw IOException("bad audio header")
            val rate = input.readInt()
            input.readInt() // channels (always 2)
            input.readInt() // bits (always 16)

            val minBuf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
            track = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build(),
                )
                .setAudioFormat(
                    AudioFormat.Builder().setSampleRate(rate).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build(),
                )
                .setBufferSizeInBytes(minBuf * 2)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track.play()

            val maxBacklog = rate * 4 * 120 / 1000  // bytes in 120 ms of stereo PCM16
            var buf = ByteArray(8192)
            while (running) {
                val len = input.readInt()
                if (len <= 0 || len > (1 shl 20)) throw IOException("bad audio chunk")
                if (len > buf.size) buf = ByteArray(len)
                input.readFully(buf, 0, len)
                if (input.available() > maxBacklog) continue  // behind: drop this chunk to catch up
                track.write(buf, 0, len)
            }
        } catch (_: Exception) {
            // connection closed or audio unavailable: the video keeps working
        } finally {
            running = false
            runCatching { socket?.close() }
            track?.let { runCatching { it.stop() }; it.release() }
        }
    }

    companion object {
        const val AUDIO_PORT = 48768
    }
}
