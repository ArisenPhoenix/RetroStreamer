package com.archstreamer.client.ui.menu

import com.archstreamer.client.ui.NavSection

/**
 * Where the cursor is. Two columns: the drawer list of sections, and the options the
 * highlighted section owns.
 *
 * The option is tracked by id rather than index because option lists change with state
 * (the conditional current-password field, the custom-layout chip, disc labels).
 */
data class MenuFocus(
    val inOptions: Boolean = false,
    val section: NavSection = NavSection.Client,
    val optionId: String = "",
    /** A real text field holds IME focus: arrows belong to the cursor, not the navigator. */
    val editing: Boolean = false,
)

/** Things only the composition can do. Everything else is a focus change. */
sealed interface MenuEffect {
    data object OpenDrawer : MenuEffect
    data object CloseDrawer : MenuEffect

    /** Back parks focus on the hamburger as the exit arm. */
    data object FocusHamburger : MenuEffect

    /**
     * Hand IME focus to the real field behind [optionId] so a keyboard can type.
     *
     * There is no matching release effect: [MenuFocus.editing] going false is itself the
     * signal to give the field up, so paths that never run through the navigator (the
     * drawer opening, a pane swap, returning to play) release it too.
     */
    data class FocusField(val optionId: String) : MenuEffect
}

data class NavOutcome(val focus: MenuFocus, val effect: MenuEffect? = null)

/**
 * Pure focus movement over a built menu.
 *
 * Drawer column: Up/Down shift sections, Right or South enters the highlighted one.
 * Options column: the option gets first refusal on Left/Right, then Up/Down walk the
 * section's rows, wrapping at either end so the cursor cannot leave the section it entered.
 * Left (declined) and East return to the drawer.
 *
 * Moving is not side-effect free: offering Left/Right to an option is how a pill changes
 * chips and a slider nudges, so those callbacks fire from [move].
 */
object MenuNavigator {
    /** Cursor parked on [section] in the drawer column. */
    fun atDrawer(menu: List<MenuSection>, section: NavSection): MenuFocus {
        val sections = menu.filter { it.enabled }
        val landing = sections.firstOrNull { it.id == section }?.id
            ?: sections.firstOrNull()?.id
            ?: section
        return MenuFocus(inOptions = false, section = landing)
    }

    /** Repair a focus whose section or option no longer exists. */
    fun resolve(menu: List<MenuSection>, focus: MenuFocus): MenuFocus {
        val sections = menu.filter { it.enabled }
        if (sections.isEmpty()) return focus.copy(inOptions = false)
        val section = sections.firstOrNull { it.id == focus.section }
            ?: return MenuFocus(section = sections.first().id)
        if (!focus.inOptions) return focus
        val rows = section.focusable
        if (rows.any { it.id == focus.optionId }) return focus
        val first = rows.firstOrNull()?.id
            ?: return focus.copy(inOptions = false, optionId = "")
        return focus.copy(optionId = first)
    }

    /** Null only when there is no navigable menu at all. */
    fun move(menu: List<MenuSection>, focus: MenuFocus, dir: NavDir): NavOutcome? {
        val sections = menu.filter { it.enabled }
        if (sections.isEmpty()) return null
        val here = resolve(menu, focus)
        val at = sections.indexOfFirst { it.id == here.section }.coerceAtLeast(0)
        val current = sections[at]

        if (!here.inOptions) {
            return when (dir) {
                NavDir.Up, NavDir.Down -> {
                    val stepped = if (dir == NavDir.Up) at - 1 else at + 1
                    val next = sections[wrap(stepped, sections.size)]
                    NavOutcome(here.copy(section = next.id, optionId = ""))
                }
                NavDir.Right -> enter(current, here)
                // Nothing sits left of the drawer; swallow it so it cannot reach the game.
                NavDir.Left -> NavOutcome(here)
            }
        }

        if (dir == NavDir.Left || dir == NavDir.Right) {
            val option = current.option(here.optionId)
            if (option != null && option.onHorizontal(dir)) return NavOutcome(here)
            return if (dir == NavDir.Left) {
                NavOutcome(here.copy(inOptions = false), MenuEffect.OpenDrawer)
            } else {
                NavOutcome(here)
            }
        }

        current.nextOptionId(here.optionId, dir)?.let { next ->
            return NavOutcome(here.copy(optionId = next))
        }

        // The cursor stays in the section it entered: off the bottom it comes back to the
        // top, and off the top to the bottom. Left or the menu button is the way out, so a
        // held direction can never walk the cursor somewhere off screen.
        val wrapped = current.edgeFocusableId(dir) ?: return NavOutcome(here)
        return NavOutcome(here.copy(optionId = wrapped))
    }

    /** South / Enter: enter a section from the drawer, or fire the focused option. */
    fun activate(menu: List<MenuSection>, focus: MenuFocus): NavOutcome? {
        val sections = menu.filter { it.enabled }
        if (sections.isEmpty()) return null
        val here = resolve(menu, focus)
        val current = sections.firstOrNull { it.id == here.section } ?: return null
        if (!here.inOptions) return enter(current, here)
        val option = current.option(here.optionId) ?: return NavOutcome(here)
        option.onActivate()
        return if (option.takesTyping) {
            NavOutcome(here.copy(editing = true), MenuEffect.FocusField(option.id))
        } else {
            NavOutcome(here)
        }
    }

    /** East / B: stop typing, else step back out to the drawer column. */
    fun leaveOptions(focus: MenuFocus): NavOutcome? {
        if (focus.editing) return NavOutcome(focus.copy(editing = false))
        if (!focus.inOptions) return null
        return NavOutcome(focus.copy(inOptions = false), MenuEffect.OpenDrawer)
    }

    /**
     * Entering always reveals the pane, so it closes the drawer even for a section with
     * nothing focusable (the Games list owns its own input).
     */
    private fun enter(section: MenuSection, focus: MenuFocus): NavOutcome {
        val first = section.firstFocusableId()
            ?: return NavOutcome(focus.copy(inOptions = false), MenuEffect.CloseDrawer)
        return NavOutcome(
            focus.copy(inOptions = true, optionId = first),
            MenuEffect.CloseDrawer,
        )
    }

    private fun wrap(index: Int, size: Int): Int = ((index % size) + size) % size
}
