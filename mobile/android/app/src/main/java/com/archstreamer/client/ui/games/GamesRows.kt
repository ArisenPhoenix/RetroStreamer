package com.archstreamer.client.ui.games

import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.ui.GamesState
import com.archstreamer.client.ui.menu.NavDir

/** Recents is a group header like any other, so it navigates like one. */
const val RECENTS_GROUP = "Recents"

/** Group title for offerings that arrive without a system. */
private const val OTHER_GROUP = "Other"

/**
 * One navigable line of the Games pane, in the order it is drawn.
 *
 * The pane owns its own cursor because the catalog is not a fixed set of options: the rows
 * come and go with the filter and with which groups are open.
 */
sealed interface GamesRow {
    /** Identity the cursor is kept by, so it survives a rebuild of the list. */
    val key: String

    /** The filter field, pinned above the list and reached by moving up off the top. */
    data object Filter : GamesRow {
        override val key = FILTER_KEY
    }

    data class Header(val system: String, val count: Int, val expanded: Boolean) : GamesRow {
        override val key: String get() = "hdr-$system"
    }

    data class Entry(val system: String, val game: GameInfo) : GamesRow {
        override val key: String get() = "game-$system-${game.id}"
    }

    companion object {
        const val FILTER_KEY = "games-filter"
    }
}

/**
 * Every row the Games pane draws, filter first.
 *
 * Navigation and rendering both read this, so the cursor cannot land somewhere the pane
 * does not draw.
 */
fun gamesRows(games: GamesState): List<GamesRow> {
    val needle = games.filter.trim().lowercase()
    val matches = if (needle.isEmpty()) {
        games.items
    } else {
        games.items.filter { it.matches(needle) }
    }
    val byId = matches.associateBy { it.id }
    val recents = games.recentGameIds.mapNotNull { byId[it] }
    val grouped = matches
        .groupBy { it.groupName() }
        .toList()
        .sortedBy { it.first.lowercase() }
    return buildList {
        add(GamesRow.Filter)
        if (recents.isNotEmpty()) {
            addGroup(RECENTS_GROUP, recents, games.expandedSystems)
        }
        grouped.forEach { (system, entries) -> addGroup(system, entries, games.expandedSystems) }
    }
}

private fun MutableList<GamesRow>.addGroup(
    system: String,
    entries: List<GameInfo>,
    expandedSystems: Set<String>,
) {
    val expanded = system in expandedSystems
    add(GamesRow.Header(system, entries.size, expanded))
    if (expanded) entries.forEach { add(GamesRow.Entry(system, it)) }
}

/**
 * Where the cursor is.
 *
 * With nothing chosen yet it sits on the first group — Recents when there is one — rather
 * than on the filter, so entering the pane lands on something worth pressing.
 */
fun gamesCursor(rows: List<GamesRow>, cursorKey: String?): GamesRow? =
    rows.firstOrNull { it.key == cursorKey }
        ?: rows.firstOrNull { it !is GamesRow.Filter }
        ?: rows.firstOrNull()

/**
 * The neighbour in [dir], wrapping around the ends of the list so a held direction keeps
 * the cursor on a drawn row. Left and Right are not ours.
 */
fun stepGamesCursor(rows: List<GamesRow>, cursorKey: String?, dir: NavDir): GamesRow? {
    if (dir != NavDir.Up && dir != NavDir.Down) return null
    val here = gamesCursor(rows, cursorKey) ?: return null
    val at = rows.indexOfFirst { it.key == here.key }
    if (at < 0) return null
    val stepped = if (dir == NavDir.Up) at - 1 else at + 1
    return rows[(stepped + rows.size) % rows.size]
}

private fun GameInfo.matches(needle: String): Boolean =
    title().lowercase().contains(needle) ||
        version.lowercase().contains(needle) ||
        systemName.lowercase().contains(needle) ||
        systemKey.lowercase().contains(needle)

private fun GameInfo.groupName(): String =
    systemName.ifBlank { systemKey.ifBlank { OTHER_GROUP } }
