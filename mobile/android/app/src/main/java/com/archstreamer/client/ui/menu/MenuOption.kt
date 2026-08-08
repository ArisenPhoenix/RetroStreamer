package com.archstreamer.client.ui.menu

import androidx.compose.runtime.Composable

/** D-pad / arrow direction after key or hat mapping. */
enum class NavDir { Up, Down, Left, Right }

/**
 * One row inside a section.
 *
 * Options own their own horizontal behaviour and activation, so the navigator never has
 * to know which kind of control it is looking at: it offers Left/Right to the option
 * first and only treats the press as movement when the option declines it.
 *
 * [enabled] false means "draw it, but never take focus" — copy, dividers, and greyed-out
 * controls all use it, so the reachable set can never drift from what is on screen.
 */
sealed interface MenuOption {
    /** Unique within its section. Focus is tracked by id so option lists may change with state. */
    val id: String
    val title: String
    val enabled: Boolean

    /**
     * Left / Right first refusal. True keeps focus here (the option changed its value);
     * false hands the press back to the navigator as movement.
     */
    fun onHorizontal(dir: NavDir): Boolean = false

    /** South / Enter / tap. */
    fun onActivate() {}

    /** True when activating should hand IME focus to the real field for typing. */
    val takesTyping: Boolean get() = false

    /** Free text. Activating focuses the real field so a keyboard can type into it. */
    data class TextInput(
        override val id: String,
        override val title: String,
        val value: String,
        val onChange: (String) -> Unit,
        val placeholder: String = "",
        val supporting: String = "",
        val isError: Boolean = false,
        val readOnly: Boolean = false,
        val minLines: Int = 1,
        val maxLines: Int = 1,
        override val enabled: Boolean = true,
    ) : MenuOption {
        override val takesTyping: Boolean get() = !readOnly
    }

    /** Masked text. Same behaviour as [TextInput] with the value hidden. */
    data class PasswordInput(
        override val id: String,
        override val title: String,
        val value: String,
        val onChange: (String) -> Unit,
        val supporting: String = "",
        override val enabled: Boolean = true,
    ) : MenuOption {
        override val takesTyping: Boolean get() = true
    }

    /**
     * On/off switch. South toggles; Left/Right set Off/On directly and decline when
     * already there, so Left on an unchecked flipper escapes back to the drawer.
     */
    data class Flipper(
        override val id: String,
        override val title: String,
        val checked: Boolean,
        val onChange: (Boolean) -> Unit,
        val subtitle: String = "",
        override val enabled: Boolean = true,
    ) : MenuOption {
        override fun onActivate() {
            if (enabled) onChange(!checked)
        }

        override fun onHorizontal(dir: NavDir): Boolean {
            if (!enabled) return false
            val want = dir == NavDir.Right
            if (want == checked) return false
            onChange(want)
            return true
        }
    }

    /**
     * Exclusive choice row. Left/Right move the selection and decline at the ends, so
     * Left off the first chip leaves the row.
     */
    data class Pill(
        override val id: String,
        override val title: String,
        val choices: List<Choice>,
        val selectedIndex: Int,
        override val enabled: Boolean = true,
    ) : MenuOption {
        data class Choice(val label: String, val onSelect: () -> Unit)

        override fun onHorizontal(dir: NavDir): Boolean {
            if (!enabled || choices.isEmpty()) return false
            val next = if (dir == NavDir.Right) selectedIndex + 1 else selectedIndex - 1
            if (next !in choices.indices) return false
            choices[next].onSelect()
            return true
        }
    }

    /**
     * Continuous value. Left/Right always nudge (clamped at the ends), so East is the
     * way out of a slider rather than Left.
     */
    data class Slider(
        override val id: String,
        override val title: String,
        val value: Float,
        val range: ClosedFloatingPointRange<Float>,
        val onChange: (Float) -> Unit,
        val steps: Int = 0,
        override val enabled: Boolean = true,
    ) : MenuOption {
        /** Twenty notches across the range. */
        val step: Float
            get() = ((range.endInclusive - range.start) / NOTCHES).coerceAtLeast(0.01f)

        override fun onHorizontal(dir: NavDir): Boolean {
            if (!enabled) return false
            val delta = if (dir == NavDir.Right) step else -step
            onChange((value + delta).coerceIn(range.start, range.endInclusive))
            return true
        }

        private companion object {
            const val NOTCHES = 20f
        }
    }

    /** Plain button. Covers Connect, Ensure Host, Resync A/V, Leave session, and friends. */
    data class Action(
        override val id: String,
        override val title: String,
        val onRun: () -> Unit,
        val style: Style = Style.Filled,
        override val enabled: Boolean = true,
    ) : MenuOption {
        enum class Style { Filled, Tonal, Outlined, Text }

        override fun onActivate() {
            if (enabled) onRun()
        }
    }

    /** Static copy, headings, and status lines. Never takes focus. */
    data class Note(
        override val id: String,
        override val title: String,
        val style: Style = Style.Body,
        val emphasis: Emphasis = Emphasis.Normal,
    ) : MenuOption {
        enum class Style { Header, SubHeader, Body, Small }
        enum class Emphasis { Normal, Muted, Primary, Error }

        override val enabled: Boolean get() = false
    }

    /** Horizontal rule between groups of options. */
    data class Divider(override val id: String) : MenuOption {
        override val title: String get() = ""
        override val enabled: Boolean get() = false
    }

    /**
     * Bespoke content the option types do not describe yet (selectable lists, spinners).
     * Touch-only: it is drawn in place but skipped by the navigator.
     */
    class Custom(
        override val id: String,
        val content: @Composable () -> Unit,
    ) : MenuOption {
        override val title: String get() = ""
        override val enabled: Boolean get() = false
    }
}
