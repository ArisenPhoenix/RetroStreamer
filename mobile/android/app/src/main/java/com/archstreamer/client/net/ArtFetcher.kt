package com.archstreamer.client.net

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import java.io.File

/**
 * Fetch display art (boxart → grid → icon) from the host.
 *
 * Files are cached under [cacheDir]/art/. Sync is gated on the catalog offerings
 * revision (same idea as GameList / blocks caches): unchanged revision → load from
 * disk only, zero TCP. When sync is needed, one control connection does GameList
 * then ArtAssetRequests for missing keys only.
 */
object ArtFetcher {
    private val displayRoles = listOf("boxart", "grid", "icon")

    /**
     * Load one asset from disk, or fetch if missing (one short-lived connection per role).
     */
    fun loadBitmap(
        host: String,
        controlPort: Int,
        assetKey: String,
        cacheDir: File,
    ): Bitmap? {
        if (assetKey.isBlank()) return null
        readCacheBitmap(cacheDir, assetKey)?.let { return it }
        for (role in displayRoles) {
            val bitmap = runCatching {
                ControlConnection(host, controlPort).use { conn ->
                    conn.connect()
                    conn.send(PacketCodec.artAssetRequest(assetKey, role, cachedSha256 = ""))
                    when (val packet = conn.receive()) {
                        is IncomingPacket.Art -> {
                            val art = packet.value
                            if (!art.found || art.data.isEmpty()) return@use null
                            writeCache(cacheDir, assetKey, art.data)
                            BitmapFactory.decodeByteArray(art.data, 0, art.data.size)
                        }
                        else -> null
                    }
                }
            }.getOrNull()
            if (bitmap != null) return bitmap
        }
        return readCacheBitmap(cacheDir, assetKey)
    }

    /**
     * Prefetch catalog art. [catalogRevision] gates network: when it matches the
     * last successful sync and every key is on disk, only decode from disk.
     * Otherwise one TCP fills gaps (GameList then ArtAsset* on the same socket).
     */
    fun prefetchAll(
        host: String,
        controlPort: Int,
        games: List<GameInfo>,
        cacheDir: File,
        catalogRevision: Long,
        isActive: () -> Boolean,
        onLoaded: (assetKey: String, bitmap: Bitmap) -> Unit,
    ) {
        val keys = games.map { it.assetKey }.filter { it.isNotBlank() }.distinct()
        if (keys.isEmpty() || !isActive()) return

        val marker = artSyncMarker(cacheDir)
        val syncedRev = readArtSyncRevision(marker)
        val missing = keys.filter { readCacheBytes(cacheDir, it) == null }

        if (missing.isNotEmpty() && isActive()) {
            runCatching {
                syncMissingBatch(host, controlPort, missing, cacheDir, catalogRevision, isActive)
            }
        } else if (catalogRevision != 0L && syncedRev != catalogRevision) {
            // Everything on disk; just advance the sync marker with the catalog.
            writeArtSyncRevision(marker, catalogRevision)
        } else if (missing.isEmpty() && syncedRev == catalogRevision && catalogRevision != 0L) {
            // Warm path: zero TCP.
        }

        for (assetKey in keys) {
            if (!isActive()) return
            val bitmap = readCacheBitmap(cacheDir, assetKey) ?: continue
            onLoaded(assetKey, bitmap)
        }
    }

    private fun syncMissingBatch(
        host: String,
        controlPort: Int,
        missingKeys: List<String>,
        cacheDir: File,
        catalogRevision: Long,
        isActive: () -> Boolean,
    ) {
        if (missingKeys.isEmpty()) {
            writeArtSyncRevision(artSyncMarker(cacheDir), catalogRevision)
            return
        }

        ControlConnection(host, controlPort).use { conn ->
            conn.connect()
            // GameList first so the host keeps the socket open for ArtAsset* loop.
            conn.send(PacketCodec.gameListRequest(catalogRevision))
            when (val packet = conn.receive()) {
                is IncomingPacket.Catalog -> Unit
                is IncomingPacket.Error -> return
                else -> return
            }
            for (assetKey in missingKeys) {
                if (!isActive()) return
                for (role in displayRoles) {
                    if (!isActive()) return
                    conn.send(PacketCodec.artAssetRequest(assetKey, role, cachedSha256 = ""))
                    when (val packet = conn.receive()) {
                        is IncomingPacket.Art -> {
                            val art = packet.value
                            if (!art.found) continue
                            if (art.data.isNotEmpty()) {
                                writeCache(cacheDir, assetKey, art.data)
                                break
                            }
                        }
                        is IncomingPacket.Error -> return
                        else -> continue
                    }
                }
            }
        }
        writeArtSyncRevision(artSyncMarker(cacheDir), catalogRevision)
    }

    private fun artSyncMarker(cacheDir: File): File =
        File(File(cacheDir, "art"), ".art_sync_catalog_revision")

    private fun readArtSyncRevision(marker: File): Long {
        if (!marker.isFile) return 0L
        return runCatching { marker.readText().trim().toLong() }.getOrDefault(0L)
    }

    private fun writeArtSyncRevision(marker: File, revision: Long) {
        runCatching {
            marker.parentFile?.mkdirs()
            marker.writeText("$revision\n")
        }
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
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
    }

    private fun writeCache(cacheDir: File, assetKey: String, data: ByteArray) {
        val file = cacheFile(cacheDir, assetKey)
        file.parentFile?.mkdirs()
        file.writeBytes(data)
    }
}
