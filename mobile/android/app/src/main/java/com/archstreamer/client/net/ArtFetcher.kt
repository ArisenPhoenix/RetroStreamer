package com.archstreamer.client.net

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import java.io.File
import java.security.MessageDigest

/**
 * Fetch display art (boxart → grid → icon) from the host.
 * Prefetch uses one control TCP (GameList then ArtAsset*), matching the desktop client.
 * Cache is revalidated via content SHA-256 so host artwork updates replace stale client files.
 */
object ArtFetcher {
    private val displayRoles = listOf("boxart", "grid", "icon")

    /**
     * Load one asset (revalidate against host). Opens a short-lived control connection.
     */
    fun loadBitmap(
        host: String,
        controlPort: Int,
        assetKey: String,
        cacheDir: File,
    ): Bitmap? {
        if (assetKey.isBlank()) return null
        return runCatching {
            ControlConnection(host, controlPort).use { conn ->
                conn.connect()
                fetchFirstRole(conn, assetKey, cacheDir)
            }
        }.getOrNull() ?: readCacheBitmap(cacheDir, assetKey)
    }

    /**
     * Background-friendly batch: one TCP, catalog probe, then revalidate all art.
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
        if (keys.isEmpty() || !isActive()) return

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
                for (assetKey in keys) {
                    if (!isActive()) return@use
                    val bitmap = fetchFirstRole(conn, assetKey, cacheDir)
                        ?: readCacheBitmap(cacheDir, assetKey)
                        ?: continue
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
        val cachedBytes = readCacheBytes(cacheDir, assetKey)
        val cachedSha = cachedBytes?.let { sha256Prefixed(it) }.orEmpty()
        for (role in displayRoles) {
            conn.send(PacketCodec.artAssetRequest(assetKey, role, cachedSha))
            when (val packet = conn.receive()) {
                is IncomingPacket.Art -> {
                    val art = packet.value
                    if (!art.found) continue
                    if (art.data.isNotEmpty()) {
                        writeCache(cacheDir, assetKey, art.data)
                        return BitmapFactory.decodeByteArray(art.data, 0, art.data.size)
                    }
                    // Not-modified (hash match) or empty body from older host with found=true.
                    if (cachedBytes != null &&
                        (art.contentSha256.isEmpty() || art.contentSha256 == cachedSha)
                    ) {
                        return BitmapFactory.decodeByteArray(cachedBytes, 0, cachedBytes.size)
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

    private fun readCacheBytes(cacheDir: File, assetKey: String): ByteArray? {
        val file = cacheFile(cacheDir, assetKey)
        if (!file.isFile || file.length() == 0L) return null
        return runCatching { file.readBytes() }.getOrNull()?.takeIf { it.isNotEmpty() }
    }

    private fun readCacheBitmap(cacheDir: File, assetKey: String): Bitmap? {
        val bytes = readCacheBytes(cacheDir, assetKey) ?: return null
        return runCatching { BitmapFactory.decodeByteArray(bytes, 0, bytes.size) }.getOrNull()
    }

    private fun writeCache(cacheDir: File, assetKey: String, data: ByteArray) {
        val file = cacheFile(cacheDir, assetKey)
        file.parentFile?.mkdirs()
        runCatching { file.writeBytes(data) }
    }

    private fun sha256Prefixed(data: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(data)
        val hex = buildString(digest.size * 2) {
            for (byte in digest) {
                append("%02x".format(byte))
            }
        }
        return "sha256:$hex"
    }
}
