package com.screenlink.app

import android.os.Handler
import android.os.Looper
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/**
 * TCP client for the ScreenLink control channel (JSON Lines).
 *
 * - Every connection gets a generation number; an old connection that ends late can no
 *   longer close or overwrite the status of a newer one.
 * - Errors stay visible: the final status is the error message, not "disconnected".
 * - Messages are sent on one background thread, so they arrive in the order they were sent.
 */
class TcpClient(
    private val port: Int = DEFAULT_PORT,
    private val hello: () -> String = { Messages.hello() },
    private val update: (status: String, reply: String?) -> Unit,
) {
    private val main = Handler(Looper.getMainLooper())
    private val lock = Any()
    private val sender: ExecutorService = Executors.newSingleThreadExecutor()
    private var socket: Socket? = null
    private var writer: BufferedWriter? = null
    private var generation = 0

    private fun publish(status: String, reply: String? = null) {
        main.post { update(status, reply) }
    }

    fun connect(host: String) {
        val myGeneration = synchronized(lock) {
            closeLocked()
            ++generation
        }
        if (host.isBlank()) {
            publish("$STATUS_ERROR: أدخل عنوان IP")
            return
        }
        publish(STATUS_CONNECTING)
        thread(name = "screenlink-connect-$myGeneration") {
            var error: String? = null
            val s = Socket()
            try {
                s.tcpNoDelay = true
                s.connect(InetSocketAddress(host, port), CONNECT_TIMEOUT_MS)
                val w = BufferedWriter(OutputStreamWriter(s.getOutputStream(), Charsets.UTF_8))
                val stillCurrent = synchronized(lock) {
                    if (myGeneration == generation) {
                        socket = s
                        writer = w
                        true
                    } else {
                        false
                    }
                }
                if (!stillCurrent) return@thread
                publish(STATUS_CONNECTED)
                send(hello())
                val reader = BufferedReader(InputStreamReader(s.getInputStream(), Charsets.UTF_8))
                while (true) {
                    val line = reader.readLine() ?: break
                    if (!isCurrent(myGeneration)) break
                    publish(STATUS_CONNECTED, line)
                }
            } catch (e: Exception) {
                error = e.message ?: e.javaClass.simpleName
            } finally {
                runCatching { s.close() }
                val wasCurrent = synchronized(lock) {
                    if (myGeneration == generation) {
                        closeLocked()
                        true
                    } else {
                        false
                    }
                }
                // Only the newest connection reports; a replaced or user-closed one stays silent.
                if (wasCurrent) publish(if (error != null) "$STATUS_ERROR: $error" else STATUS_DISCONNECTED)
            }
        }
    }

    fun send(message: String) {
        if (sender.isShutdown) return
        sender.execute {
            try {
                synchronized(lock) {
                    val w = writer ?: throw IllegalStateException("غير متصل")
                    w.write(message)
                    w.newLine()
                    w.flush()
                }
            } catch (e: Exception) {
                publish("$STATUS_ERROR: ${e.message ?: e.javaClass.simpleName}")
            }
        }
    }

    /** User pressed "disconnect". */
    fun disconnect() {
        synchronized(lock) {
            ++generation
            closeLocked()
        }
        publish(STATUS_DISCONNECTED)
    }

    /** Screen is going away: close everything without posting UI updates. */
    fun shutdown() {
        synchronized(lock) {
            ++generation
            closeLocked()
        }
        sender.shutdownNow()
    }

    private fun isCurrent(gen: Int) = synchronized(lock) { gen == generation }

    private fun closeLocked() {
        runCatching { socket?.close() }
        socket = null
        writer = null
    }

    companion object {
        const val DEFAULT_PORT = 48765
        const val CONNECT_TIMEOUT_MS = 4000
        const val STATUS_CONNECTING = "جارٍ الاتصال…"
        const val STATUS_CONNECTED = "CONNECTED"
        const val STATUS_DISCONNECTED = "غير متصل"
        const val STATUS_ERROR = "خطأ"
    }
}
