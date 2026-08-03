package com.archstreamer.client.net

import com.archstreamer.client.protocol.Protocol
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.util.concurrent.ConcurrentHashMap

/**
 * Android port of desktop HostDiscoveryBrowser (UDP ASDISC01 / ASDISCQ1 on 45550).
 * Binds the discovery port so hosts can reply to probes (they answer 45550, not ephemeral).
 */
data class DiscoveredHost(
    val username: String,
    val address: String,
    val controlPort: Int = Protocol.DEFAULT_CONTROL_PORT,
    val inputPort: Int = Protocol.DEFAULT_INPUT_PORT,
    val lastSeenMs: Long = System.currentTimeMillis(),
)

class HostDiscovery(
    private val discoveryPort: Int = Protocol.DEFAULT_DISCOVERY_PORT,
) : AutoCloseable {
    private val socket: DatagramSocket = DatagramSocket(null).apply {
        reuseAddress = true
        broadcast = true
        soTimeout = 50
        bind(java.net.InetSocketAddress(discoveryPort))
    }

    private val hosts = ConcurrentHashMap<String, DiscoveredHost>()
    private var seedHosts: List<String> = emptyList()
    private var nextSubnetProbeMs: Long = 0L

    fun setSeedHosts(hosts: List<String>) {
        seedHosts = hosts.map { it.trim() }.filter { it.isNotEmpty() }
    }

    fun hosts(): List<DiscoveredHost> = hosts.values.sortedBy { it.address }

    fun poll() {
        drainAnnouncements()
        probeLan()
    }

    fun expireOlderThan(maxAgeMs: Long = 8_000L) {
        val now = System.currentTimeMillis()
        hosts.entries.removeIf { now - it.value.lastSeenMs > maxAgeMs }
    }

    private fun drainAnnouncements() {
        val buf = ByteArray(2048)
        while (true) {
            val packet = DatagramPacket(buf, buf.size)
            try {
                socket.receive(packet)
            } catch (_: SocketTimeoutException) {
                break
            } catch (_: Exception) {
                break
            }
            val bytes = packet.data.copyOf(packet.length)
            val announcement = parseAnnouncement(bytes) ?: continue
            val address = packet.address.hostAddress ?: continue
            noteAnnouncement(announcement, address)
        }
    }

    private fun noteAnnouncement(announcement: HostAnnouncement, address: String) {
        val key = "${announcement.username}\u0000$address"
        hosts[key] = DiscoveredHost(
            username = announcement.username,
            address = address,
            controlPort = announcement.controlPort,
            inputPort = announcement.inputPort,
            lastSeenMs = System.currentTimeMillis(),
        )
    }

    private fun probeLan() {
        val now = System.currentTimeMillis()
        val subnetDue = now >= nextSubnetProbeMs
        val targets = probeTargets(subnetDue)
        if (subnetDue) {
            nextSubnetProbeMs = now + 5_000L
        }
        if (targets.isEmpty()) return
        val probe = serializeProbe()
        for (target in targets) {
            try {
                val addr = InetAddress.getByName(target)
                socket.send(DatagramPacket(probe, probe.size, addr, discoveryPort))
            } catch (_: Exception) {
            }
        }
    }

    private fun probeTargets(includeSubnet: Boolean): List<String> {
        val targets = LinkedHashSet<String>()
        fun add(address: String) {
            if (address.isBlank() || isLoopback(address) || !isIpv4(address)) return
            targets.add(address)
        }
        for (seed in seedHosts) add(seed)

        if (!includeSubnet) return targets.toList()

        for (local in localIpv4Addresses()) {
            val host = ipv4ToInt(local) ?: continue
            // Skip docker / libvirt / CGNAT-like ranges (same as desktop). Keep 10.x for WireGuard.
            if ((host ushr 16) == 0xac11) continue // 172.17.x.x
            if ((host and 0xffffff00.toInt()) == 0xc0a87a00.toInt()) continue // 192.168.122.x
            if ((host ushr 24) == 100) continue // 100.x.x.x
            val base = host and 0xffffff00.toInt()
            for (octet in 1..254) {
                val candidate = base or octet
                if (candidate == host) continue
                add(intToIpv4(candidate))
            }
        }
        return targets.toList()
    }

    override fun close() {
        runCatching { socket.close() }
    }

    private data class HostAnnouncement(
        val username: String,
        val controlPort: Int,
        val inputPort: Int,
    )

    companion object {
        private const val MAGIC = "ASDISC01"
        private const val PROBE_MAGIC = "ASDISCQ1"

        fun serializeProbe(): ByteArray = PROBE_MAGIC.toByteArray(StandardCharsets.US_ASCII)

        private fun parseAnnouncement(bytes: ByteArray): HostAnnouncement? {
            val magicBytes = MAGIC.toByteArray(StandardCharsets.US_ASCII)
            if (bytes.size < magicBytes.size + 6) return null
            for (i in magicBytes.indices) {
                if (bytes[i] != magicBytes[i]) return null
            }
            val buf = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN)
            buf.position(magicBytes.size)
            val nameLen = buf.short.toInt() and 0xffff
            if (nameLen <= 0 || buf.remaining() < nameLen + 4) return null
            val nameBytes = ByteArray(nameLen)
            buf.get(nameBytes)
            val username = String(nameBytes, StandardCharsets.UTF_8)
            if (username.isEmpty()) return null
            val controlPort = buf.short.toInt() and 0xffff
            val inputPort = buf.short.toInt() and 0xffff
            return HostAnnouncement(username, controlPort, inputPort)
        }

        fun localIpv4Addresses(): List<String> {
            val out = ArrayList<String>()
            try {
                val ifaces = NetworkInterface.getNetworkInterfaces() ?: return out
                for (iface in ifaces) {
                    if (!iface.isUp || iface.isLoopback) continue
                    for (addr in iface.inetAddresses) {
                        if (addr is Inet4Address && !addr.isLoopbackAddress) {
                            out.add(addr.hostAddress ?: continue)
                        }
                    }
                }
            } catch (_: Exception) {
            }
            return out
        }

        fun ipv4SameSubnet24(left: String, right: String): Boolean {
            val a = ipv4ToInt(left) ?: return false
            val b = ipv4ToInt(right) ?: return false
            return (a and 0xffffff00.toInt()) == (b and 0xffffff00.toInt())
        }

        fun preferDiscoveredHost(hosts: List<DiscoveredHost>): DiscoveredHost? {
            val local = localIpv4Addresses()
            for (host in hosts) {
                if (isLoopback(host.address)) continue
                for (localAddress in local) {
                    if (ipv4SameSubnet24(host.address, localAddress)) return host
                }
            }
            return hosts.firstOrNull { !isLoopback(it.address) }
        }

        fun isLoopback(address: String): Boolean {
            val host = ipv4ToInt(address) ?: return address.startsWith("127.")
            return (host ushr 24) == 127
        }

        private fun isIpv4(address: String): Boolean = ipv4ToInt(address) != null

        private fun ipv4ToInt(text: String): Int? {
            val parts = text.split('.')
            if (parts.size != 4) return null
            var value = 0
            for (part in parts) {
                val octet = part.toIntOrNull() ?: return null
                if (octet !in 0..255) return null
                value = (value shl 8) or octet
            }
            return value
        }

        private fun intToIpv4(value: Int): String {
            return "${(value ushr 24) and 0xff}.${(value ushr 16) and 0xff}." +
                "${(value ushr 8) and 0xff}.${value and 0xff}"
        }
    }
}
