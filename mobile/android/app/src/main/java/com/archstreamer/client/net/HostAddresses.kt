package com.archstreamer.client.net

import java.net.ConnectException
import java.net.Inet4Address
import java.net.Inet6Address
import java.net.InetAddress
import java.net.NoRouteToHostException
import java.net.PortUnreachableException
import java.net.SocketTimeoutException
import java.net.UnknownHostException

/** Host IP / Alt IP helpers for Client tab connect fallback. */
object HostAddresses {
    private val IPV4 =
        Regex("""^((25[0-5]|2[0-4]\d|[01]?\d?\d)\.){3}(25[0-5]|2[0-4]\d|[01]?\d?\d)$""")

    /** True when [value] looks like an IPv4 or IPv6 address (not a hostname). */
    fun looksLikeIp(value: String): Boolean {
        val trimmed = value.trim()
        if (trimmed.isEmpty()) return false
        if (IPV4.matches(trimmed)) return true
        if (':' !in trimmed) return false
        return try {
            when (val addr = InetAddress.getByName(trimmed)) {
                is Inet4Address, is Inet6Address -> true
                else -> false
            }
        } catch (_: Exception) {
            false
        }
    }

    /**
     * Ordered connect targets: primary Host IP, then Alt IP when set, valid, and different.
     * Blank Alt is ignored. Invalid Alt is skipped (caller may surface a separate warning).
     */
    fun connectCandidates(primary: String, alt: String): List<String> {
        val first = primary.trim()
        val second = alt.trim()
        val out = ArrayList<String>(2)
        if (first.isNotEmpty()) {
            out.add(first)
        }
        if (second.isNotEmpty() &&
            looksLikeIp(second) &&
            !second.equals(first, ignoreCase = true)
        ) {
            out.add(second)
        }
        return out
    }

    /** TCP reachability failures — safe to try Alt IP. Auth/protocol errors are not. */
    fun isReachabilityFailure(error: Throwable): Boolean {
        var cur: Throwable? = error
        while (cur != null) {
            when (cur) {
                is ConnectException,
                is SocketTimeoutException,
                is NoRouteToHostException,
                is UnknownHostException,
                is PortUnreachableException,
                -> return true
            }
            val msg = cur.message.orEmpty().lowercase()
            if (msg.contains("econnrefused") ||
                msg.contains("etimedout") ||
                msg.contains("enotunreach") ||
                msg.contains("network is unreachable") ||
                msg.contains("failed to connect") ||
                msg.contains("connection refused") ||
                msg.contains("timed out")
            ) {
                return true
            }
            cur = cur.cause
        }
        return false
    }
}
