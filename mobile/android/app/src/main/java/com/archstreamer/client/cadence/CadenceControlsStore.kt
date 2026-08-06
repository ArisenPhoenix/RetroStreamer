package com.archstreamer.client.cadence

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import java.io.File

/**
 * Local SQLite mirror of profile `controls.sqlite` (`user_controls` table).
 * Canonical on-device store for button maps + overlay profiles; SharedPreferences
 * may cache overlays at runtime after load.
 */
class CadenceControlsStore(context: Context) :
    SQLiteOpenHelper(context, DB_NAME, null, DB_VERSION) {

    private val dbFile: File = context.getDatabasePath(DB_NAME)

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """
            CREATE TABLE IF NOT EXISTS user_controls (
              username TEXT NOT NULL,
              kind TEXT NOT NULL DEFAULT 'button_map',
              document_json TEXT NOT NULL,
              version INTEGER NOT NULL DEFAULT 1,
              updated_at INTEGER NOT NULL DEFAULT 0,
              PRIMARY KEY (username, kind)
            )
            """.trimIndent(),
        )
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        // v1 schema is additive-only so far.
    }

    fun upsertControls(
        username: String,
        kind: String = KIND_BUTTON_MAP,
        documentJson: String,
        version: Int = 1,
    ): Boolean {
        if (username.isBlank() || documentJson.isBlank()) return false
        val now = System.currentTimeMillis() / 1000L
        writableDatabase.execSQL(
            """
            INSERT INTO user_controls(username, kind, document_json, version, updated_at)
            VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(username, kind) DO UPDATE SET
              document_json=excluded.document_json,
              version=excluded.version,
              updated_at=excluded.updated_at
            """.trimIndent(),
            arrayOf(username, kind, documentJson, version, now),
        )
        return true
    }

    fun findControls(username: String, kind: String = KIND_BUTTON_MAP): String? {
        if (username.isBlank()) return null
        readableDatabase.rawQuery(
            """
            SELECT document_json FROM user_controls
            WHERE username = ? AND kind = ?
            LIMIT 1
            """.trimIndent(),
            arrayOf(username, kind),
        ).use { cursor ->
            if (!cursor.moveToFirst()) return null
            return cursor.getString(0)
        }
    }

    fun deleteControlsForUser(username: String) {
        if (username.isBlank()) return
        writableDatabase.delete("user_controls", "username = ?", arrayOf(username))
    }

    /**
     * Export this username's rows into a standalone controls.sqlite byte array
     * (same schema as host profile packs).
     */
    fun exportPackBytes(username: String): ByteArray {
        require(username.isNotBlank())
        val tmp = File.createTempFile("controls_pack_", ".sqlite")
        try {
            tmp.delete()
            SQLiteDatabase.openOrCreateDatabase(tmp, null).use { pack ->
                pack.execSQL(
                    """
                    CREATE TABLE IF NOT EXISTS user_controls (
                      username TEXT NOT NULL,
                      kind TEXT NOT NULL DEFAULT 'button_map',
                      document_json TEXT NOT NULL,
                      version INTEGER NOT NULL DEFAULT 1,
                      updated_at INTEGER NOT NULL DEFAULT 0,
                      PRIMARY KEY (username, kind)
                    )
                    """.trimIndent(),
                )
                readableDatabase.rawQuery(
                    """
                    SELECT username, kind, document_json, version, updated_at
                    FROM user_controls WHERE username = ?
                    """.trimIndent(),
                    arrayOf(username),
                ).use { cursor ->
                    while (cursor.moveToNext()) {
                        pack.execSQL(
                            """
                            INSERT INTO user_controls(username, kind, document_json, version, updated_at)
                            VALUES (?, ?, ?, ?, ?)
                            """.trimIndent(),
                            arrayOf(
                                cursor.getString(0),
                                cursor.getString(1),
                                cursor.getString(2),
                                cursor.getInt(3),
                                cursor.getLong(4),
                            ),
                        )
                    }
                }
            }
            val bytes = tmp.readBytes()
            require(bytes.size <= MAX_PACK_BYTES) { "controls pack too large" }
            return bytes
        } finally {
            tmp.delete()
        }
    }

    /**
     * Replace local rows for [username] with those from a host/profile pack.
     * Ignores rows for other usernames.
     */
    fun importPackBytes(username: String, bytes: ByteArray): Boolean {
        if (username.isBlank() || bytes.isEmpty() || bytes.size > MAX_PACK_BYTES) return false
        val tmp = File.createTempFile("controls_import_", ".sqlite")
        try {
            tmp.writeBytes(bytes)
            SQLiteDatabase.openDatabase(
                tmp.absolutePath,
                null,
                SQLiteDatabase.OPEN_READONLY,
            ).use { pack ->
                pack.rawQuery(
                    """
                    SELECT username, kind, document_json, version, updated_at
                    FROM user_controls
                    """.trimIndent(),
                    null,
                ).use { cursor ->
                    deleteControlsForUser(username)
                    while (cursor.moveToNext()) {
                        val rowUser = cursor.getString(0) ?: continue
                        if (!rowUser.equals(username, ignoreCase = true)) continue
                        upsertControls(
                            username = username,
                            kind = cursor.getString(1) ?: continue,
                            documentJson = cursor.getString(2) ?: continue,
                            version = cursor.getInt(3),
                        )
                    }
                }
            }
            return true
        } catch (_: Exception) {
            return false
        } finally {
            tmp.delete()
        }
    }

    companion object {
        const val DB_NAME = "archstreamer_cadence.sqlite"
        const val DB_VERSION = 1
        const val KIND_BUTTON_MAP = "button_map"
        const val KIND_OVERLAY_PROFILES = "overlay_profiles"
        const val DEFAULT_USERNAME = "_default"
        const val MAX_PACK_BYTES = 2 * 1024 * 1024
    }
}
