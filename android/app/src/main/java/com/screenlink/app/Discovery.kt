package com.screenlink.app

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/** A computer running the Windows program, found on the local network. */
data class FoundHost(val name: String, val address: String)

/** Finds computers on the same Wi-Fi by UDP broadcast (the PC answers on port 48767). */
object Discovery {
    const val PORT = 48767

    suspend fun search(timeoutMs: Int = 1500): List<FoundHost> = withContext(Dispatchers.IO) {
        val found = LinkedHashMap<String, FoundHost>()
        runCatching {
            DatagramSocket().use { socket ->
                socket.broadcast = true
                socket.soTimeout = 300
                val payload = "SCREENLINK_DISCOVER".toByteArray()
                for (target in broadcastAddresses()) {
                    runCatching { socket.send(DatagramPacket(payload, payload.size, target, PORT)) }
                }
                val buf = ByteArray(1024)
                val end = System.currentTimeMillis() + timeoutMs
                while (System.currentTimeMillis() < end) {
                    val packet = DatagramPacket(buf, buf.size)
                    try {
                        socket.receive(packet)
                    } catch (_: SocketTimeoutException) {
                        continue
                    }
                    val json = runCatching { JSONObject(String(packet.data, 0, packet.length, Charsets.UTF_8)) }.getOrNull()
                    if (json?.optString("type") == "HERE") {
                        val ip = packet.address.hostAddress ?: continue
                        found[ip] = FoundHost(json.optString("host_name", ip), ip)
                    }
                }
            }
        }
        found.values.toList()
    }

    /** The general broadcast address plus each network's own (some routers drop the general one). */
    fun broadcastAddresses(): List<InetAddress> {
        val result = mutableListOf(InetAddress.getByName("255.255.255.255"))
        runCatching {
            for (nif in NetworkInterface.getNetworkInterfaces()) {
                if (!nif.isUp || nif.isLoopback) continue
                for (addr in nif.interfaceAddresses) addr.broadcast?.let { result.add(it) }
            }
        }
        return result.distinct()
    }
}
