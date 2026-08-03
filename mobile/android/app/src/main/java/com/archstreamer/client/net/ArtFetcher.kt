package com.archstreamer.client.net

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import java.io.File

/**
 * Fetch display art (boxart → grid → icon) from the host.
 * Prefetch uses one control TCP (GameList then ArtAsset*), matching the desktop client.
 */
object ArtFetcher {
    private val displayRoles = listOf("boxart", "grid", "icon")

    /**
     * Load one asset (cache first). Opens a short-lived control connection when missing.
     */
    fun loadBitmap(
        host: String,
        controlPort: Int,
        assetKey: String,
        cacheDir: File,
    ): Bitmap? {
        if (assetKey.isBlank()) return null
        readCache(cacheDir, assetKey)?.let { return it }
        return runCatching {
            ControlConnection(host, controlPort).use { conn ->
                conn.connect()
                fetchFirstRole(conn, assetKey, cacheDir)
            }
        }.getOrNull()
    }

    /**
     * Background-friendly batch: one TCP, catalog probe, then missing art only.
     * [onLoaded] may be called from the worker thread.
     */
    fun prefetchAll(
        host: String,
        controlPort: Int,
        games: List<GameInfo>,
        cacheDir: File,
        isActive: () -> Boolean,
        onLoaded: (assetKey: String, bitmap: Bitmap) -> Unit,
    ) {
        val keys = games.map { it.assetKey }.filter { it.isNotBlank() }.distinct()
        val pending = mutableListOf<String>()
        for (key in keys) {
            if (!isActive()) return
            val cached = readCache(cacheDir, key)
            if (cached != null) {
                onLoaded(key, cached)
            } else {
                pending.add(key)
            }
        }
        if (pending.isEmpty() || !isActive()) return

        runCatching {
            ControlConnection(host, controlPort).use { conn ->
                conn.connect()
                // Host accepts ArtAsset after GameList on the same socket (before Hello).
                conn.send(PacketCodec.gameListRequest(revision = 0L))
                when (val catalog = conn.receive()) {
                    is IncomingPacket.Catalog -> Unit
                    is IncomingPacket.Error -> return@use
                    else -> return@use
                }
                for (assetKey in pending) {
                    if (!isActive()) return@use
                    val bitmap = fetchFirstRole(conn, assetKey, cacheDir) ?: continue
                    onLoaded(assetKey, bitmap)
                }
            }
        }
    }

    private fun fetchFirstRole(
        conn: ControlConnection,
        assetKey: String,
        cacheDir: File,
    ): Bitmap? {
        for (role in displayRoles) {
            conn.send(PacketCodec.artAssetRequest(assetKey, role))
            when (val packet = conn.receive()) {
                is IncomingPacket.Art -> {
                    val art = packet.value
                    if (art.found && art.data.isNotEmpty()) {
                        writeCache(cacheDir, assetKey, art.data)
                        return BitmapFactory.decodeByteArray(art.data, 0, art.data.size)
                    }
                }
                is IncomingPacket.Error -> return null
                else -> continue
            }
        }
        return null
    }

    private fun cacheFile(cacheDir: File, assetKey: String): File {
        val safe = assetKey.replace(Regex("[^A-Za-z0-9._-]"), "_")
        return File(File(cacheDir, "art"), "$safe.bin")
    }

    private fun readCache(cacheDir: File, assetKey: String): Bitmap? {
        val file = cacheFile(cacheDir, assetKey)
        if (!file.isFile || file.length() == 0L) return null
        return runCatching { BitmapFactory.decodeFile(file.absolutePath) }.getOrNull()
    }

    private fun writeCache(cacheDir: File, assetKey: String, data: ByteArray) {
        val file = cacheFile(cacheDir, assetKey)
        file.parentFile?.mkdirs()
        runCatching { file.writeBytes(data) }
    }
}
