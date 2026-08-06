package com.archstreamer.client.net

import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import com.archstreamer.client.protocol.Protocol
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * Framed TCP control channel (same framing as common TcpStream).
 * Call from a background thread / coroutine dispatcher — not the UI thread.
 *
 * Send and receive use separate locks so heartbeats / ClientSessionLeave can
 * flush while the control poller is blocked in tryReceive (TCP is full-duplex).
 */
class ControlConnection(
    private val host: String,
    private val port: Int,
) : AutoCloseable {
    private val socket = Socket()
    private lateinit var input: BufferedInputStream
    private lateinit var output: BufferedOutputStream
    private val sendLock = Any()
    private val receiveLock = Any()
    private val endpointLabel = "$host:$port"

    fun connect(timeoutMs: Int = 8_000) {
        ClientFileLog.conn("TCP connect begin $endpointLabel timeout=${timeoutMs}ms")
        try {
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress(host, port), timeoutMs)
            input = BufferedInputStream(socket.getInputStream())
            output = BufferedOutputStream(socket.getOutputStream())
            ClientFileLog.conn(
                "TCP connected $endpointLabel local=${socket.localPort}",
            )
        } catch (t: Throwable) {
            ClientFileLog.conn("TCP connect failed $endpointLabel: ${t.message ?: t}")
            throw t
        }
    }

    fun send(packet: ByteArray) {
        synchronized(sendLock) {
            check(!socket.isClosed) { "control socket closed" }
            output.write(packet)
            output.flush()
        }
    }

    /** True while the TCP control socket can still send. */
    fun isConnected(): Boolean = socket.isConnected && !socket.isClosed && !socket.isOutputShutdown

    fun receive(): IncomingPacket {
        synchronized(receiveLock) {
            return receiveLocked()
        }
    }

    /**
     * Poll one framed packet. Returns null on read timeout (no data yet).
     * Used by the in-session control loop so SoftKeyboardRequest can arrive
     * while heartbeats keep flowing on the same socket.
     */
    fun tryReceive(timeoutMs: Int = 500): IncomingPacket? {
        synchronized(receiveLock) {
            val previous = socket.soTimeout
            socket.soTimeout = timeoutMs
            return try {
                receiveLocked()
            } catch (_: SocketTimeoutException) {
                null
            } finally {
                socket.soTimeout = previous
            }
        }
    }

    private fun receiveLocked(): IncomingPacket {
        val header = readFully(Protocol.HEADER_SIZE)
        val (_, type, payloadSize) = PacketCodec.parseHeader(header)
        val payload = if (payloadSize > 0) readFully(payloadSize) else ByteArray(0)
        val packet = PacketCodec.decode(type, payload)
        logInbound(packet)
        return packet
    }

    private fun logInbound(packet: IncomingPacket) {
        if (!ClientFileLog.logConnections) return
        val label = when (packet) {
            is IncomingPacket.Welcome -> "HostWelcome id=${packet.value.clientId}"
            is IncomingPacket.Seats -> "SeatAssignment seats=${packet.value.seats.size}"
            is IncomingPacket.Ready -> "SessionReady"
            is IncomingPacket.Starting -> "SessionStarting"
            is IncomingPacket.Ended -> "SessionEnded reason=${packet.value.reason}"
            is IncomingPacket.Media ->
                "MediaEndpoint video=${packet.value.videoUri.isNotBlank()} " +
                    "audio=${packet.value.audioUri.isNotBlank()}"
            is IncomingPacket.VideoPending -> "MediaVideoPending"
            is IncomingPacket.Error -> "Error: ${packet.value.message}"
            is IncomingPacket.LobbyPresenceAck -> "LobbyPresenceAck id=${packet.clientId}"
            is IncomingPacket.PasswordChangeRequired -> "PasswordChangeRequired"
            is IncomingPacket.SoftKeyboard -> "SoftKeyboardRequest"
            is IncomingPacket.Catalog -> "GameList games=${packet.value.games.size}"
            else -> packet::class.simpleName ?: "packet"
        }
        // Heartbeats are client→host only; skip high-frequency noise if any.
        if (label == "packet") return
        ClientFileLog.conn("TCP ← $endpointLabel $label")
    }

    private fun readFully(size: Int): ByteArray {
        val buf = ByteArray(size)
        var filled = 0
        while (filled < size) {
            val n = input.read(buf, filled, size - filled)
            if (n < 0) {
                ClientFileLog.conn("TCP disconnected $endpointLabel (EOF)")
                error("host disconnected")
            }
            filled += n
        }
        return buf
    }

    override fun close() {
        val wasOpen = runCatching { socket.isConnected && !socket.isClosed }.getOrDefault(false)
        runCatching { socket.close() }
        if (wasOpen) {
            ClientFileLog.conn("TCP closed $endpointLabel")
        }
    }
}
