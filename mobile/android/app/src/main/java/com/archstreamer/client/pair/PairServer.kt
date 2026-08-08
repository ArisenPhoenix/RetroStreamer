package com.archstreamer.client.pair

import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.nio.charset.StandardCharsets
import java.util.Collections
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

data class PairListenSession(
    val uri: String,
    val lanIp: String,
    val port: Int,
    val token: String,
    private val server: ServerSocket,
    private val future: Future<*>,
    private val closed: AtomicBoolean = AtomicBoolean(false),
) {
    fun close() {
        if (!closed.compareAndSet(false, true)) return
        runCatching { server.close() }
        future.cancel(true)
    }
}

/**
 * One-shot LAN HTTP receiver for [PairProfile] JSON.
 * QR encodes: archstreamer://pair?v=1&ip=&port=&token=
 */
object PairServer {
    private val executor = Executors.newCachedThreadPool()

    fun start(
        ttlMs: Long = 120_000L,
        onProfile: (PairProfile) -> Unit,
        onError: (String) -> Unit = {},
    ): PairListenSession {
        val lanIp = primaryLanIpv4()
            ?: throw IllegalStateException("No LAN IPv4 address found")
        val token = UUID.randomUUID().toString().replace("-", "").take(16)
        val server = ServerSocket(0)
        server.soTimeout = ttlMs.toInt().coerceIn(5_000, 300_000)
        val port = server.localPort
        val uri = "archstreamer://pair?v=1&ip=$lanIp&port=$port&token=$token"
        val future = executor.submit {
            try {
                val socket = server.accept()
                socket.soTimeout = 15_000
                handleClient(socket, token, onProfile, onError)
            } catch (err: Exception) {
                if (!server.isClosed) {
                    onError(err.message ?: err.toString())
                }
            } finally {
                runCatching { server.close() }
            }
        }
        return PairListenSession(uri, lanIp, port, token, server, future)
    }

    private fun handleClient(
        socket: Socket,
        expectedToken: String,
        onProfile: (PairProfile) -> Unit,
        onError: (String) -> Unit,
    ) {
        socket.use { sock ->
            val input = sock.getInputStream()
            val output = sock.getOutputStream()
            val headerBytes = readUntilDoubleCrlf(input)
            val headerText = String(headerBytes, StandardCharsets.US_ASCII)
            val contentLength = Regex("(?i)Content-Length:\\s*(\\d+)")
                .find(headerText)
                ?.groupValues
                ?.getOrNull(1)
                ?.toIntOrNull()
                ?: 0
            val auth = Regex("(?i)Authorization:\\s*Bearer\\s+(\\S+)")
                .find(headerText)
                ?.groupValues
                ?.getOrNull(1)
                .orEmpty()
            val body = if (contentLength > 0) {
                val buf = ByteArray(contentLength)
                var off = 0
                while (off < contentLength) {
                    val n = input.read(buf, off, contentLength - off)
                    if (n < 0) break
                    off += n
                }
                String(buf, 0, off, StandardCharsets.UTF_8)
            } else {
                ""
            }
            if (auth != expectedToken) {
                writeHttp(output, 401, """{"ok":false,"error":"bad token"}""")
                onError("Pair rejected: bad token")
                return
            }
            if (!headerText.startsWith("POST ")) {
                writeHttp(output, 405, """{"ok":false,"error":"POST required"}""")
                onError("Pair rejected: not POST")
                return
            }
            try {
                val profile = PairProfile.fromJson(body)
                writeHttp(output, 200, """{"ok":true}""")
                onProfile(profile)
            } catch (err: Exception) {
                writeHttp(
                    output,
                    400,
                    """{"ok":false,"error":${jsonString(err.message ?: "bad json")}}""",
                )
                onError(err.message ?: "bad profile json")
            }
        }
    }

    private fun readUntilDoubleCrlf(input: java.io.InputStream): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        var prev = 0
        var count = 0
        while (true) {
            val b = input.read()
            if (b < 0) break
            out.write(b)
            if (b == '\n'.code && prev == '\r'.code) {
                count++
                if (count == 2) break
            } else if (b != '\r'.code) {
                count = 0
            }
            prev = b
            if (out.size() > 64 * 1024) throw IllegalStateException("HTTP header too large")
        }
        return out.toByteArray()
    }

    private fun writeHttp(output: java.io.OutputStream, code: Int, body: String) {
        val bytes = body.toByteArray(StandardCharsets.UTF_8)
        val status = when (code) {
            200 -> "200 OK"
            400 -> "400 Bad Request"
            401 -> "401 Unauthorized"
            405 -> "405 Method Not Allowed"
            else -> "$code Error"
        }
        val header =
            "HTTP/1.1 $status\r\n" +
                "Content-Type: application/json\r\n" +
                "Content-Length: ${bytes.size}\r\n" +
                "Connection: close\r\n\r\n"
        output.write(header.toByteArray(StandardCharsets.US_ASCII))
        output.write(bytes)
        output.flush()
    }

    private fun jsonString(value: String): String =
        "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

    fun primaryLanIpv4(): String? {
        val ifaces = Collections.list(NetworkInterface.getNetworkInterfaces())
        val candidates = mutableListOf<String>()
        for (iface in ifaces) {
            if (!iface.isUp || iface.isLoopback) continue
            for (addr in Collections.list(iface.inetAddresses)) {
                if (addr !is Inet4Address || addr.isLoopbackAddress) continue
                val host = addr.hostAddress ?: continue
                candidates += host
            }
        }
        // Prefer common private ranges.
        return candidates.firstOrNull { it.startsWith("192.168.") }
            ?: candidates.firstOrNull { it.startsWith("10.") }
            ?: candidates.firstOrNull { it.startsWith("172.") }
            ?: candidates.firstOrNull()
    }

    fun awaitQuietly(session: PairListenSession, timeoutMs: Long = 1_000L) {
        runCatching {
            session.close()
            // Give accept thread a moment to exit.
            TimeUnit.MILLISECONDS.sleep(timeoutMs.coerceAtMost(200))
        }
    }
}

data class PairTarget(
    val ip: String,
    val port: Int,
    val token: String,
) {
    companion object {
        fun parseUri(uri: String): PairTarget {
            val trimmed = uri.trim()
            require(trimmed.startsWith("archstreamer://pair")) {
                "Not an ArchStreamer pair QR"
            }
            val query = trimmed.substringAfter('?', missingDelimiterValue = "")
            val params = query.split('&').mapNotNull { part ->
                val i = part.indexOf('=')
                if (i <= 0) null else part.substring(0, i) to part.substring(i + 1)
            }.toMap()
            val ip = params["ip"].orEmpty()
            val port = params["port"]?.toIntOrNull() ?: 0
            val token = params["token"].orEmpty()
            require(ip.isNotBlank() && port in 1..65535 && token.isNotBlank()) {
                "Pair QR missing ip/port/token"
            }
            return PairTarget(ip, port, token)
        }
    }
}
