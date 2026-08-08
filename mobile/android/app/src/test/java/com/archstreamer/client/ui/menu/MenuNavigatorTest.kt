package com.archstreamer.client.ui.menu

import com.archstreamer.client.ui.NavSection
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class MenuNavigatorTest {
    private fun note(id: String) = MenuOption.Note(id = id, title = id)

    private fun action(id: String, onRun: () -> Unit = {}) =
        MenuOption.Action(id = id, title = id, onRun = onRun)

    private fun section(
        id: NavSection,
        vararg options: MenuOption,
        enabled: Boolean = true,
    ) = MenuSection(id = id, title = id.title, options = options.toList(), enabled = enabled)

    @Test
    fun `down moves to the next focusable option and skips copy`() {
        val menu = listOf(
            section(NavSection.Client, action("a"), note("blurb"), action("b")),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "a")

        val out = MenuNavigator.move(menu, focus, NavDir.Down)

        assertEquals("b", out?.focus?.optionId)
        assertEquals(NavSection.Client, out?.focus?.section)
    }

    @Test
    fun `down past the last option wraps to the top of the same section`() {
        val menu = listOf(
            section(NavSection.Client, action("a"), action("b")),
            section(NavSection.Remote, action("elsewhere")),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "b")

        val out = MenuNavigator.move(menu, focus, NavDir.Down)

        assertEquals(NavSection.Client, out?.focus?.section)
        assertEquals("a", out?.focus?.optionId)
        assertTrue(out?.focus?.inOptions == true)
    }

    @Test
    fun `up past the first option wraps to the bottom of the same section`() {
        val menu = listOf(
            section(NavSection.Client, action("a"), action("b")),
            section(NavSection.Remote, action("elsewhere")),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "a")

        val out = MenuNavigator.move(menu, focus, NavDir.Up)

        assertEquals(NavSection.Client, out?.focus?.section)
        assertEquals("b", out?.focus?.optionId)
    }

    @Test
    fun `a section with one option keeps the cursor on it`() {
        val menu = listOf(
            section(NavSection.Client, action("only"), note("copy")),
            section(NavSection.Remote, action("elsewhere")),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "only")

        val out = MenuNavigator.move(menu, focus, NavDir.Down)

        assertEquals(NavSection.Client, out?.focus?.section)
        assertEquals("only", out?.focus?.optionId)
    }

    @Test
    fun `disabled sections are skipped in the drawer column`() {
        val menu = listOf(
            section(NavSection.Client, action("a")),
            section(NavSection.Games, action("b"), enabled = false),
            section(NavSection.Remote, action("c")),
        )
        val focus = MenuFocus(section = NavSection.Client)

        val out = MenuNavigator.move(menu, focus, NavDir.Down)

        assertEquals(NavSection.Remote, out?.focus?.section)
    }

    @Test
    fun `right off an option that cannot turn takes the next row`() {
        val menu = listOf(
            section(NavSection.Client, action("a"), action("b")),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "a")

        val out = MenuNavigator.move(menu, focus, NavDir.Right)

        assertEquals("b", out?.focus?.optionId)
        assertNull(out?.effect)
    }

    @Test
    fun `right off the last row enters the next section`() {
        val menu = listOf(
            section(NavSection.Client, action("a")),
            section(NavSection.Remote, note("header"), action("target")),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "a")

        val out = MenuNavigator.move(menu, focus, NavDir.Right)

        assertEquals(NavSection.Remote, out?.focus?.section)
        assertEquals("target", out?.focus?.optionId)
        assertTrue(out?.focus?.inOptions == true)
        // The pane has to swap to the section the cursor moved into.
        assertEquals(MenuEffect.CloseDrawer, out?.effect)
    }

    @Test
    fun `pill takes right until the last chip then moves on`() {
        var picked = -1
        val choices = List(3) { index ->
            MenuOption.Pill.Choice("chip$index") { picked = index }
        }
        val menu = listOf(
            section(
                NavSection.Stream,
                MenuOption.Pill(id = "pill", title = "Pill", choices = choices, selectedIndex = 1),
                action("below"),
            ),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Stream, optionId = "pill")

        val moved = MenuNavigator.move(menu, focus, NavDir.Right)
        assertEquals(2, picked)
        assertEquals("pill", moved?.focus?.optionId)

        // Out of chips, so Right goes forwards to the row below instead.
        val atEnd = listOf(
            section(
                NavSection.Stream,
                MenuOption.Pill(id = "pill", title = "Pill", choices = choices, selectedIndex = 2),
                action("below"),
            ),
        )
        picked = -1
        val declined = MenuNavigator.move(atEnd, focus, NavDir.Right)
        assertEquals(-1, picked)
        assertEquals("below", declined?.focus?.optionId)
    }

    @Test
    fun `pill left off the first chip returns to the drawer`() {
        val choices = List(2) { index -> MenuOption.Pill.Choice("chip$index") {} }
        val menu = listOf(
            section(
                NavSection.Stream,
                MenuOption.Pill(id = "pill", title = "Pill", choices = choices, selectedIndex = 0),
            ),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Stream, optionId = "pill")

        val out = MenuNavigator.move(menu, focus, NavDir.Left)

        assertFalse(out!!.focus.inOptions)
        assertEquals(MenuEffect.OpenDrawer, out.effect)
    }

    @Test
    fun `slider always takes horizontal and clamps at the range end`() {
        var value = 1.0f
        fun menuAt(current: Float) = listOf(
            section(
                NavSection.Controls,
                MenuOption.Slider(
                    id = "opacity",
                    title = "Opacity",
                    value = current,
                    range = 0.2f..1.0f,
                    onChange = { value = it },
                ),
            ),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Controls, optionId = "opacity")

        // One notch is a twentieth of the range.
        val down = MenuNavigator.move(menuAt(1.0f), focus, NavDir.Left)
        assertEquals(0.96f, value, 0.0001f)
        assertTrue(down!!.focus.inOptions)
        assertNull(down.effect)

        // At the top it clamps but still keeps the press, so Left is never an escape.
        val up = MenuNavigator.move(menuAt(1.0f), focus, NavDir.Right)
        assertEquals(1.0f, value, 0.0001f)
        assertTrue(up!!.focus.inOptions)
    }

    @Test
    fun `flipper takes right only when it changes`() {
        var checked = false
        val menu = listOf(
            section(
                NavSection.Settings,
                MenuOption.Flipper(
                    id = "log",
                    title = "Log controls",
                    checked = false,
                    onChange = { checked = it },
                ),
            ),
        )
        val focus = MenuFocus(inOptions = true, section = NavSection.Settings, optionId = "log")

        val on = MenuNavigator.move(menu, focus, NavDir.Right)
        assertTrue(checked)
        assertTrue(on!!.focus.inOptions)

        // Already off: Left has nothing to do, so it leaves the column.
        val out = MenuNavigator.move(menu, focus, NavDir.Left)
        assertFalse(out!!.focus.inOptions)
    }

    @Test
    fun `right from the drawer enters the first option and closes the drawer`() {
        val menu = listOf(
            section(NavSection.Client, note("header"), action("connect")),
        )
        val focus = MenuFocus(section = NavSection.Client)

        val out = MenuNavigator.move(menu, focus, NavDir.Right)

        assertTrue(out!!.focus.inOptions)
        assertEquals("connect", out.focus.optionId)
        assertEquals(MenuEffect.CloseDrawer, out.effect)
    }

    @Test
    fun `activate runs an action and asks to type in a field`() {
        var ran = false
        val menu = listOf(
            section(
                NavSection.Client,
                action("connect") { ran = true },
                MenuOption.TextInput(
                    id = "host",
                    title = "Host IP",
                    value = "",
                    onChange = {},
                ),
            ),
        )
        val onAction = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "connect")
        MenuNavigator.activate(menu, onAction)
        assertTrue(ran)

        val onField = MenuFocus(inOptions = true, section = NavSection.Client, optionId = "host")
        val out = MenuNavigator.activate(menu, onField)
        assertEquals(MenuEffect.FocusField("host"), out?.effect)
        assertTrue(out!!.focus.editing)
    }

    @Test
    fun `east stops typing before it leaves the column`() {
        val typing = MenuFocus(
            inOptions = true,
            section = NavSection.Client,
            optionId = "host",
            editing = true,
        )

        // Dropping editing is the release: the cursor stays on the field's row.
        val released = MenuNavigator.leaveOptions(typing)
        assertNull(released?.effect)
        assertTrue(released!!.focus.inOptions)
        assertFalse(released.focus.editing)
        assertEquals("host", released.focus.optionId)

        val left = MenuNavigator.leaveOptions(released.focus)
        assertFalse(left!!.focus.inOptions)
        assertEquals(MenuEffect.OpenDrawer, left.effect)
    }

    @Test
    fun `resolve repairs a stale option and an unavailable section`() {
        val menu = listOf(
            section(NavSection.Client, action("a")),
        )

        val staleOption = MenuFocus(
            inOptions = true,
            section = NavSection.Client,
            optionId = "gone",
        )
        assertEquals("a", MenuNavigator.resolve(menu, staleOption).optionId)

        val staleSection = MenuFocus(inOptions = true, section = NavSection.Profile)
        val repaired = MenuNavigator.resolve(menu, staleSection)
        assertEquals(NavSection.Client, repaired.section)
        assertFalse(repaired.inOptions)
    }
}
