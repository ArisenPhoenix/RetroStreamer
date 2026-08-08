package com.archstreamer.client.pair

import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets

object PairClient {
    fun push(target: PairTarget, profile: PairProfile, timeoutMs: Int = 12_000): String {
        val url = URL("http://${target.ip}:${target.port}/pair")
        val conn = (url.openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = timeoutMs
            readTimeout = timeoutMs
            doOutput = true
            setRequestProperty("Content-Type", "application/json; charset=utf-8")
            setRequestProperty("Authorization", "Bearer ${target.token}")
        }
        try {
            val body = profile.toJson().toByteArray(StandardCharsets.UTF_8)
            conn.setFixedLengthStreamingMode(body.size)
            conn.outputStream.use { it.write(body) }
            val code = conn.responseCode
            val text = (if (code in 200..299) conn.inputStream else conn.errorStream)
                ?.bufferedReader(StandardCharsets.UTF_8)
                ?.use { it.readText() }
                .orEmpty()
            if (code !in 200..299) {
                throw IllegalStateException("Pair push failed HTTP $code: $text")
            }
            return text.ifBlank { """{"ok":true}""" }
        } finally {
            conn.disconnect()
        }
    }
}
