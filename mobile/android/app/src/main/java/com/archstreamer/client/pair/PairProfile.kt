package com.archstreamer.client.pair

import com.archstreamer.client.protocol.MediaQualityTier
import com.archstreamer.client.protocol.MediaStreamBitrate
import com.archstreamer.client.protocol.MediaStreamFeel
import com.archstreamer.client.protocol.MediaStreamSize

/** Connection profile pushed over LAN after donor validates host login. */
data class PairProfile(
    val host: String,
    val altHost: String,
    val controlPort: String,
    val inputPort: String,
    val username: String,
    val password: String,
    val streamQuality: Int,
    val streamBitrate: Int,
    val streamSize: Int,
    val streamFeel: Int,
    val remoteSshHost: String = "",
    val remoteSshUser: String = "",
    val remoteSshPassword: String = "",
    val remoteSshPort: String = "22",
    val remoteDirectory: String = "",
    val remoteRomRoot: String = "",
    val remoteBinary: String = "./host_runner",
    val remoteStartScript: String = "",
    val remoteGpu: String = "",
    val remoteBaseControlPort: String = "",
    val remoteBaseInputPort: String = "",
) {
    fun toJson(): String = buildString {
        append('{')
        appendJsonField("v", 1)
        append(',')
        appendJsonField("host", host)
        append(',')
        appendJsonField("altHost", altHost)
        append(',')
        appendJsonField("controlPort", controlPort)
        append(',')
        appendJsonField("inputPort", inputPort)
        append(',')
        appendJsonField("username", username)
        append(',')
        appendJsonField("password", password)
        append(',')
        appendJsonField("streamQuality", streamQuality)
        append(',')
        appendJsonField("streamBitrate", streamBitrate)
        append(',')
        appendJsonField("streamSize", streamSize)
        append(',')
        appendJsonField("streamFeel", streamFeel)
        append(',')
        appendJsonField("remoteSshHost", remoteSshHost)
        append(',')
        appendJsonField("remoteSshUser", remoteSshUser)
        append(',')
        appendJsonField("remoteSshPassword", remoteSshPassword)
        append(',')
        appendJsonField("remoteSshPort", remoteSshPort)
        append(',')
        appendJsonField("remoteDirectory", remoteDirectory)
        append(',')
        appendJsonField("remoteRomRoot", remoteRomRoot)
        append(',')
        appendJsonField("remoteBinary", remoteBinary)
        append(',')
        appendJsonField("remoteStartScript", remoteStartScript)
        append(',')
        appendJsonField("remoteGpu", remoteGpu)
        append(',')
        appendJsonField("remoteBaseControlPort", remoteBaseControlPort)
        append(',')
        appendJsonField("remoteBaseInputPort", remoteBaseInputPort)
        append('}')
    }

    companion object {
        fun fromJson(raw: String): PairProfile {
            val map = parseSimpleJsonObject(raw)
            return PairProfile(
                host = map["host"].orEmpty(),
                altHost = map["altHost"].orEmpty(),
                controlPort = map["controlPort"].orEmpty(),
                inputPort = map["inputPort"].orEmpty(),
                username = map["username"].orEmpty(),
                password = map["password"].orEmpty(),
                streamQuality = map["streamQuality"]?.toIntOrNull()
                    ?: MediaQualityTier.Medium.id,
                streamBitrate = map["streamBitrate"]?.toIntOrNull()
                    ?: MediaStreamBitrate.Kbps3500.id,
                streamSize = map["streamSize"]?.toIntOrNull()
                    ?: MediaStreamSize.P720.id,
                streamFeel = map["streamFeel"]?.toIntOrNull()
                    ?: MediaStreamFeel.LowLatency.id,
                remoteSshHost = map["remoteSshHost"].orEmpty(),
                remoteSshUser = map["remoteSshUser"].orEmpty(),
                remoteSshPassword = map["remoteSshPassword"].orEmpty(),
                remoteSshPort = map["remoteSshPort"].orEmpty().ifBlank { "22" },
                remoteDirectory = map["remoteDirectory"].orEmpty(),
                remoteRomRoot = map["remoteRomRoot"].orEmpty(),
                remoteBinary = map["remoteBinary"].orEmpty().ifBlank { "./host_runner" },
                remoteStartScript = map["remoteStartScript"].orEmpty(),
                remoteGpu = map["remoteGpu"].orEmpty(),
                remoteBaseControlPort = map["remoteBaseControlPort"].orEmpty(),
                remoteBaseInputPort = map["remoteBaseInputPort"].orEmpty(),
            )
        }
    }
}

private fun StringBuilder.appendJsonField(key: String, value: String) {
    append('"').append(escapeJson(key)).append('"').append(':')
    append('"').append(escapeJson(value)).append('"')
}

private fun StringBuilder.appendJsonField(key: String, value: Int) {
    append('"').append(escapeJson(key)).append('"').append(':').append(value)
}

private fun escapeJson(value: String): String =
    buildString(value.length) {
        for (ch in value) {
            when (ch) {
                '\\' -> append("\\\\")
                '"' -> append("\\\"")
                '\n' -> append("\\n")
                '\r' -> append("\\r")
                '\t' -> append("\\t")
                else -> append(ch)
            }
        }
    }

/** Minimal object parser for our flat string/int profile (no nested objects). */
private fun parseSimpleJsonObject(raw: String): Map<String, String> {
    val body = raw.trim().removePrefix("{").removeSuffix("}").trim()
    if (body.isEmpty()) return emptyMap()
    val out = linkedMapOf<String, String>()
    var i = 0
    while (i < body.length) {
        while (i < body.length && (body[i] == ',' || body[i].isWhitespace())) i++
        if (i >= body.length) break
        require(body[i] == '"') { "expected key at $i" }
        val keyEnd = findClosingQuote(body, i + 1)
        val key = unescapeJson(body.substring(i + 1, keyEnd))
        i = keyEnd + 1
        while (i < body.length && body[i].isWhitespace()) i++
        require(i < body.length && body[i] == ':') { "expected colon after $key" }
        i++
        while (i < body.length && body[i].isWhitespace()) i++
        require(i < body.length) { "missing value for $key" }
        if (body[i] == '"') {
            val valEnd = findClosingQuote(body, i + 1)
            out[key] = unescapeJson(body.substring(i + 1, valEnd))
            i = valEnd + 1
        } else {
            val start = i
            while (i < body.length && body[i] != ',' && !body[i].isWhitespace()) i++
            out[key] = body.substring(start, i)
        }
    }
    return out
}

private fun findClosingQuote(s: String, from: Int): Int {
    var i = from
    while (i < s.length) {
        if (s[i] == '\\') {
            i += 2
            continue
        }
        if (s[i] == '"') return i
        i++
    }
    error("unterminated string")
}

private fun unescapeJson(value: String): String =
    buildString(value.length) {
        var i = 0
        while (i < value.length) {
            val ch = value[i]
            if (ch == '\\' && i + 1 < value.length) {
                when (value[i + 1]) {
                    '\\' -> append('\\')
                    '"' -> append('"')
                    'n' -> append('\n')
                    'r' -> append('\r')
                    't' -> append('\t')
                    else -> append(value[i + 1])
                }
                i += 2
            } else {
                append(ch)
                i++
            }
        }
    }
