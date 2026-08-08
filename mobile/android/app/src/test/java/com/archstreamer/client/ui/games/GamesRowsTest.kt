package com.archstreamer.client.ui.games

import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.ui.GamesState
import com.archstreamer.client.ui.menu.NavDir
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class GamesRowsTest {
    private fun game(id: String, name: String, system: String) = GameInfo(
        id = id,
        identityKey = id,
        assetKey = id,
        displayName = name,
        systemName = system,
        systemKey = system.lowercase(),
        coreName = "",
        canonicalName = name,
        version = "",
        language = "",
        region = "",
        supportsSingleplayer = true,
        supportsMultiplayer = false,
        minPlayers = 1,
        maxPlayers = 1,
        updatedAt = 0L,
        playlistDiscs = emptyList(),
    )

    private val zelda = game("1", "Zelda", "SNES")
    private val metroid = game("2", "Metroid", "SNES")
    private val sonic = game("3", "Sonic", "Genesis")

    private fun state(
        items: List<GameInfo> = listOf(zelda, metroid, sonic),
        expanded: Set<String> = emptySet(),
        recents: List<String> = emptyList(),
        filter: String = "",
    ) = GamesState(
        items = items,
        filter = filter,
        expandedSystems = expanded,
        recentGameIds = recents,
    )

    @Test
    fun `filter leads, then one header per system in name order`() {
        val rows = gamesRows(state())

        assertEquals(
            listOf(GamesRow.FILTER_KEY, "hdr-Genesis", "hdr-SNES"),
            rows.map { it.key },
        )
    }

    @Test
    fun `a collapsed group hides its games and an expanded one lists them`() {
        val rows = gamesRows(state(expanded = setOf("SNES")))

        assertEquals(
            listOf(GamesRow.FILTER_KEY, "hdr-Genesis", "hdr-SNES", "game-SNES-1", "game-SNES-2"),
            rows.map { it.key },
        )
    }

    @Test
    fun `recents come before the systems`() {
        val rows = gamesRows(state(recents = listOf("3")))

        assertEquals(
            listOf(GamesRow.FILTER_KEY, "hdr-$RECENTS_GROUP", "hdr-Genesis", "hdr-SNES"),
            rows.map { it.key },
        )
    }

    @Test
    fun `the filter drops rows that do not match`() {
        val rows = gamesRows(state(filter = "sonic"))

        assertEquals(listOf(GamesRow.FILTER_KEY, "hdr-Genesis"), rows.map { it.key })
    }

    @Test
    fun `with nothing chosen the cursor starts on the first group, not the filter`() {
        val rows = gamesRows(state())

        assertEquals("hdr-Genesis", gamesCursor(rows, null)?.key)
    }

    @Test
    fun `recents take the cursor when there are any`() {
        val rows = gamesRows(state(recents = listOf("3")))

        assertEquals("hdr-$RECENTS_GROUP", gamesCursor(rows, null)?.key)
    }

    @Test
    fun `a cursor whose row was filtered away falls back to the first group`() {
        val rows = gamesRows(state(filter = "sonic"))

        assertEquals("hdr-Genesis", gamesCursor(rows, "hdr-SNES")?.key)
    }

    @Test
    fun `down walks into an expanded group`() {
        val rows = gamesRows(state(expanded = setOf("SNES")))

        assertEquals("game-SNES-1", stepGamesCursor(rows, "hdr-SNES", NavDir.Down)?.key)
    }

    @Test
    fun `up off the top row lands on the filter`() {
        val rows = gamesRows(state())

        val up = stepGamesCursor(rows, "hdr-Genesis", NavDir.Up)

        assertTrue(up is GamesRow.Filter)
    }

    @Test
    fun `up from the filter wraps to the last row`() {
        val rows = gamesRows(state())

        assertEquals("hdr-SNES", stepGamesCursor(rows, GamesRow.FILTER_KEY, NavDir.Up)?.key)
    }

    @Test
    fun `down off the last row wraps back to the filter`() {
        val rows = gamesRows(state())

        val down = stepGamesCursor(rows, "hdr-SNES", NavDir.Down)

        assertTrue(down is GamesRow.Filter)
    }

    @Test
    fun `left and right are the pane's business, not the cursor's`() {
        val rows = gamesRows(state())

        assertNull(stepGamesCursor(rows, "hdr-SNES", NavDir.Left))
        assertNull(stepGamesCursor(rows, "hdr-SNES", NavDir.Right))
    }
}
