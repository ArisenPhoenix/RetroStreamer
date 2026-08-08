package com.archstreamer.client.ui.menu

import androidx.compose.runtime.Stable
import com.archstreamer.client.ui.ClientViewModel
import com.archstreamer.client.ui.NavSection
import com.archstreamer.client.ui.UiState

/**
 * One drawer entry plus the options it owns.
 *
 * A section is the whole options page: it decides which option comes next in a direction,
 * and at either end the navigator comes back around to this section's other edge rather
 * than walking onto another page.
 */
@Stable
class MenuSection(
    val id: NavSection,
    val title: String,
    val enabled: Boolean = true,
    provideOptions: () -> List<MenuOption>,
) {
    /** Options as given (tests and any caller that already has the list). */
    constructor(
        id: NavSection,
        title: String,
        options: List<MenuOption>,
        enabled: Boolean = true,
    ) : this(id, title, enabled, { options })

    /**
     * Built on first use, not when the menu is assembled. Every keypress used to build
     * every option of every section — the drawer only needs titles, and a move only ever
     * asks the section the cursor is in.
     */
    val options: List<MenuOption> by lazy(LazyThreadSafetyMode.NONE, provideOptions)

    /** Options that can hold the cursor, in visual order. */
    val focusable: List<MenuOption> by lazy(LazyThreadSafetyMode.NONE) {
        options.filter { it.enabled }
    }

    fun option(optionId: String): MenuOption? = options.firstOrNull { it.id == optionId }

    fun firstFocusableId(): String? = focusable.firstOrNull()?.id

    fun lastFocusableId(): String? = focusable.lastOrNull()?.id

    fun edgeFocusableId(dir: NavDir): String? =
        if (dir == NavDir.Up) lastFocusableId() else firstFocusableId()

    /**
     * The next focusable option in [dir], or null at the end of the section — the navigator
     * wraps from there. An unknown [fromId] (the option went away with a state change) lands
     * on the first focusable option instead of falling out of the section.
     */
    fun nextOptionId(fromId: String, dir: NavDir): String? {
        val rows = focusable
        if (rows.isEmpty()) return null
        val at = rows.indexOfFirst { it.id == fromId }
        if (at < 0) return rows.first().id
        val next = if (dir == NavDir.Up) at - 1 else at + 1
        return rows.getOrNull(next)?.id
    }
}

/**
 * One class per drawer entry. Owns which options exist for the current state, so the
 * renderer and the navigator can never disagree about what is reachable.
 */
interface SectionSpec {
    val id: NavSection

    /** False hides the entry entirely (Client and Profile are gone during play). */
    fun isAvailable(state: UiState): Boolean = true

    /** False greys the entry and skips it while navigating (Games before Connect). */
    fun isEnabled(state: UiState): Boolean = true

    fun options(state: UiState, vm: ClientViewModel): List<MenuOption>

    fun build(state: UiState, vm: ClientViewModel): MenuSection =
        MenuSection(
            id = id,
            title = id.title,
            enabled = isEnabled(state),
        ) {
            val started = System.nanoTime()
            options(state, vm).also { vm.noteOptionsBuilt(id, it.size, started) }
        }
}
