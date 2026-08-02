package com.archstreamer.client.net

import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import com.archstreamer.client.protocol.Protocol
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Framed TCP control channel (same framing as common TcpStream).
 * Call from a background thread / coroutine dispatcher — not the UI thread.
 */
class ControlConnection(
    private val host: String,
    private val port: Int,
) : AutoCloseable {
    private val socket = Socket()
    private lateinit var input: BufferedInputStream
    private lateinit var output: BufferedOutputStream

    fun connect(timeoutMs: Int = 8_000) {
        socket.tcpNoDelay = true
        socket.connect(InetSocketAddress(host, port), timeoutMs)
        input = BufferedInputStream(socket.getInputStream())
        output = BufferedOutputStream(socket.getOutputStream())
    }

    fun send(packet: ByteArray) {
        output.write(packet)
        output.flush()
    }

    fun receive(): IncomingPacket {
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
