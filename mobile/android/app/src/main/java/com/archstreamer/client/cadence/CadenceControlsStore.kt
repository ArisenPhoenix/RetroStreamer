package com.archstreamer.client.cadence

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

/**
 * Local SQLite mirror of host cadence `user_controls` (same column names / JSON body).
 * Overlay chrome stays in SharedPreferences; only the portable button-map document lives here.
 */
class CadenceControlsStore(context: Context) :
    SQLiteOpenHelper(context, DB_NAME, null, DB_VERSION) {

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

    companion object {
        const val DB_NAME = "archstreamer_cadence.sqlite"
        const val DB_VERSION = 1
        const val KIND_BUTTON_MAP = "button_map"
        const val DEFAULT_USERNAME = "_default"
    }
}
