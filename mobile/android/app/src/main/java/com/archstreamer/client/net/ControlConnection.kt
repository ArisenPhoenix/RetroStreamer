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

    fun connect(timeoutMs: Int = 8_000) {
        socket.tcpNoDelay = true
        socket.connect(InetSocketAddress(host, port), timeoutMs)
        input = BufferedInputStream(socket.getInputStream())
        output = BufferedOutputStream(socket.getOutputStream())
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
        return PacketCodec.decode(type, payload)
    }

    private fun readFully(size: Int): ByteArray {
        val buf = ByteArray(size)
        var filled = 0
        while (filled < size) {
            val n = input.read(buf, filled, size - filled)
            if (n < 0) error("host disconnected")
            filled += n
        }
        return buf
    }

    override fun close() {
        runCatching { socket.close() }
    }
}
