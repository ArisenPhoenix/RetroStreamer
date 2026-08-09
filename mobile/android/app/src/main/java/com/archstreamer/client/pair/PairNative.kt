package com.archstreamer.client.pair

internal object PairNative {
    init {
        System.loadLibrary("archstreamer_ds_touch")
    }

    fun parseTarget(raw: String): PairTarget? {
        val parts = nativeParseTarget(raw) ?: return null
        if (parts.size < 5) return null
        val port = parts[1].toIntOrNull() ?: return null
        val relayPort = parts[4].toIntOrNull() ?: 0
        return PairTarget(
            ip = parts[0],
            port = port,
            token = parts[2],
            relayHost = parts[3],
            relayPort = relayPort,
        )
    }

    private external fun nativeParseTarget(raw: String): Array<String>?
}
