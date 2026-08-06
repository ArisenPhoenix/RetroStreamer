package com.archstreamer.client.net

import android.content.Context
import java.io.File
import java.nio.charset.StandardCharsets
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/**
 * Append-only client log with session markers (mirrors desktop gui.log sessions).
 */
object ClientFileLog {
    const val SESSION_MARKER = "=== archstreamer_android started ==="

    private val lock = ReentrantLock()
    private var logFile: File? = null

    /** Settings → Debug → Log connections. */
    @Volatile
    var logConnections: Boolean = false

    fun init(context: Context) {
        lock.withLock {
            if (logFile != null) return
            val dir = File(context.filesDir, "archstreamer-logs")
            dir.mkdirs()
            val file = File(dir, "client.log")
            logFile = file
            appendLine(SESSION_MARKER)
            appendLine("Log file: ${file.absolutePath}")
        }
    }

    fun append(message: String) {
        appendLine("[${timestamp()}] $message")
    }

    /** Connection lifecycle line when [logConnections] is on (`conn:` prefix). */
    fun conn(message: String) {
        if (!logConnections) return
        append("conn: $message")
    }

    fun extractLastSessions(sessionCount: Int): ByteArray {
        return lock.withLock {
            val file = logFile ?: return ByteArray(0)
            if (!file.exists()) return ByteArray(0)
            val text = file.readText(StandardCharsets.UTF_8)
            extractLastSessionsText(text, SESSION_MARKER, sessionCount)
                .toByteArray(StandardCharsets.UTF_8)
        }
    }

    fun extractLastSessionsText(
        logText: String,
        sessionMarker: String,
        sessionCount: Int,
    ): String {
        if (logText.isEmpty() || sessionCount <= 0) return ""
        val starts = ArrayList<Int>()
        var pos = 0
        while (pos < logText.length) {
            val found = logText.indexOf(sessionMarker, pos)
            if (found < 0) break
            starts.add(found)
            pos = found + sessionMarker.length
        }
        if (starts.isEmpty()) {
            val tail = 256 * 1024
            return if (logText.length <= tail) logText else logText.takeLast(tail)
        }
        val take = minOf(sessionCount, starts.size)
        val begin = starts[starts.size - take]
        return logText.substring(begin)
    }

    private fun appendLine(line: String) {
        lock.withLock {
            val file = logFile ?: return
            file.appendText(line + "\n", StandardCharsets.UTF_8)
        }
    }

    private fun timestamp(): String =
        SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
}
