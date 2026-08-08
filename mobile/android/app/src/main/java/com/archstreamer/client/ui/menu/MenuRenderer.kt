package com.archstreamer.client.ui.menu

import android.content.res.Configuration
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import android.view.KeyEvent as AndroidKeyEvent
import com.archstreamer.client.ui.ClientViewModel
import com.archstreamer.client.ui.NavSection
import com.archstreamer.client.ui.UiState

/**
 * Keeps one [FocusRequester] per typing option so a [MenuEffect.FocusField] arriving from
 * the view model can hand IME focus to the real field.
 */
@Stable
class MenuFieldFocus {
    private val requesters = mutableMapOf<String, FocusRequester>()

    fun requesterFor(optionId: String): FocusRequester =
        requesters.getOrPut(optionId) { FocusRequester() }

    /** No-op when the field is not attached (option scrolled away or replaced). */
    fun request(optionId: String) {
        runCatching { requesterFor(optionId).requestFocus() }
    }
}

private val RowShape = RoundedCornerShape(12.dp)
private val DrawerRowShape = RoundedCornerShape(28.dp)

/** Text plus the plumbing every menu text field shares. */
private class FieldEditor(
    val text: String,
    val onText: (String) -> Unit,
    val keyboardOptions: KeyboardOptions,
    val modifier: Modifier,
)

/**
 * A focused field owns its text.
 *
 * The option's value is read back out of a [kotlinx.coroutines.flow.StateFlow], so it
 * arrives a frame or more after the keystroke that caused it. Feeding that lagging value
 * back into the field is what makes typing stutter, so local text answers the IME
 * immediately and [onChange] still sees every edit. Values that change from elsewhere
 * (prefill, reset, a pull from the host) are adopted once the field is not being typed in.
 */
@Composable
private fun rememberFieldEditor(
    optionId: String,
    incoming: String,
    onChange: (String) -> Unit,
    fieldFocus: MenuFieldFocus,
    onFieldFocusChanged: (String, Boolean) -> Unit,
    keyboardType: KeyboardType = KeyboardType.Unspecified,
): FieldEditor {
    var text by remember(optionId) { mutableStateOf(incoming) }
    var typing by remember(optionId) { mutableStateOf(false) }
    LaunchedEffect(optionId, incoming) {
        if (incoming != text) text = incoming
    }
    return FieldEditor(
        text = text,
        onText = { next ->
            text = next
            onChange(next)
        },
        keyboardOptions = KeyboardOptions(keyboardType = keyboardType),
        modifier = Modifier
            .fillMaxWidth()
            .focusRequester(fieldFocus.requesterFor(optionId))
            .onFocusChanged {
                val ownsFocus = it.isFocused || it.hasFocus
                typing = ownsFocus
                onFieldFocusChanged(optionId, ownsFocus)
            },
    )
}

/**
 * Every option in a section, with one focus chrome shared by all of them.
 *
 * Lazy, not a scrolling [Column]: a pane is a dozen rows of which several are text fields,
 * and composing all of them in one frame is what pushed the TV past the input dispatch
 * timeout — the app would ANR on a section that had barely appeared and the system would
 * close it. Only the rows on screen are built now.
 */
@Composable
fun MenuOptionList(
    section: MenuSection,
    focusedOptionId: String?,
    editingOptionId: String?,
    fieldFocus: MenuFieldFocus,
    onOptionTouched: (String) -> Unit,
    onFieldFocusChanged: (String, Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    val options = section.options
    val listState = rememberLazyListState()
    val configuration = LocalConfiguration.current
    val useLightweightTextRows =
        configuration.uiMode and Configuration.UI_MODE_TYPE_MASK ==
            Configuration.UI_MODE_TYPE_TELEVISION
    // A row can only ask to be revealed once it exists, so wrapping to the far end of a
    // page — where the target was never composed — has to be scrolled by the list itself.
    LaunchedEffect(section.id, focusedOptionId) {
        val target = options.indexOfFirst { it.id == focusedOptionId }
        if (target < 0) return@LaunchedEffect
        val visible = listState.layoutInfo.visibleItemsInfo
        if (visible.isNotEmpty() && visible.none { it.index == target }) {
            if (useLightweightTextRows) {
                runCatching { listState.scrollToItem(target) }
            } else {
                runCatching { listState.animateScrollToItem(target) }
            }
        }
    }
    LazyColumn(
        state = listState,
        modifier = modifier
            .fillMaxSize()
            .padding(horizontal = 12.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(options, key = { it.id }) { option ->
            MenuOptionRow(
                option = option,
                focused = option.id == focusedOptionId,
                editing = option.id == editingOptionId,
                useLightweightTextRows = useLightweightTextRows,
                fieldFocus = fieldFocus,
                onOptionTouched = onOptionTouched,
                onFieldFocusChanged = onFieldFocusChanged,
            )
        }
    }
}

@Composable
private fun MenuOptionRow(
    option: MenuOption,
    focused: Boolean,
    editing: Boolean,
    useLightweightTextRows: Boolean,
    fieldFocus: MenuFieldFocus,
    onOptionTouched: (String) -> Unit,
    onFieldFocusChanged: (String, Boolean) -> Unit,
) {
    when (option) {
        is MenuOption.Note -> NoteRow(option)
        is MenuOption.Divider -> HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
        is MenuOption.Custom -> option.content()

        is MenuOption.TextInput -> MenuRow(
            focused = focused,
            onClick = {
                onOptionTouched(option.id)
                if (option.takesTyping) onFieldFocusChanged(option.id, true)
            },
            enabled = option.enabled,
        ) {
            if (useLightweightTextRows && !editing) {
                TextInputDisplay(option.title, option.value, option.placeholder, option.supporting, option.isError)
                return@MenuRow
            }
            val editor = rememberFieldEditor(
                optionId = option.id,
                incoming = option.value,
                onChange = option.onChange,
                fieldFocus = fieldFocus,
                onFieldFocusChanged = onFieldFocusChanged,
            )
            LaunchedEffect(option.id) { fieldFocus.request(option.id) }
            if (useLightweightTextRows && editing) {
                TvHardwareTextField(
                    optionId = option.id,
                    value = option.value,
                    onValueChange = option.onChange,
                    label = option.title,
                    enabled = option.enabled,
                    isError = option.isError,
                    placeholder = option.placeholder,
                    supporting = option.supporting,
                    keyboardOptions = editor.keyboardOptions,
                    modifier = editor.modifier,
                )
            } else {
                OutlinedTextField(
                    value = editor.text,
                    onValueChange = editor.onText,
                    label = { Text(option.title) },
                    readOnly = option.readOnly,
                    singleLine = option.minLines == 1,
                    minLines = option.minLines,
                    maxLines = option.maxLines,
                    isError = option.isError,
                    enabled = option.enabled,
                    placeholder = if (option.placeholder.isNotBlank()) {
                        { Text(option.placeholder) }
                    } else {
                        null
                    },
                    supportingText = if (option.supporting.isNotBlank()) {
                        { Text(option.supporting) }
                    } else {
                        null
                    },
                    keyboardOptions = editor.keyboardOptions,
                    modifier = editor.modifier,
                )
            }
        }

        is MenuOption.PasswordInput -> MenuRow(
            focused = focused,
            onClick = {
                onOptionTouched(option.id)
                onFieldFocusChanged(option.id, true)
            },
            enabled = option.enabled,
        ) {
            if (useLightweightTextRows && !editing) {
                TextInputDisplay(
                    title = option.title,
                    value = if (option.value.isBlank()) "" else "Password set",
                    placeholder = "",
                    supporting = option.supporting,
                    isError = false,
                )
                return@MenuRow
            }
            val editor = rememberFieldEditor(
                optionId = option.id,
                incoming = option.value,
                onChange = option.onChange,
                fieldFocus = fieldFocus,
                onFieldFocusChanged = onFieldFocusChanged,
                // Password variation: no suggestion strip, no last-character preview,
                // and the IME does not learn what was typed.
                keyboardType = KeyboardType.Password,
            )
            LaunchedEffect(option.id) { fieldFocus.request(option.id) }
            if (useLightweightTextRows && editing) {
                TvHardwareTextField(
                    optionId = option.id,
                    value = option.value,
                    onValueChange = option.onChange,
                    label = option.title,
                    enabled = option.enabled,
                    supporting = option.supporting,
                    keyboardOptions = editor.keyboardOptions,
                    visualTransformation = PasswordVisualTransformation(),
                    modifier = editor.modifier,
                )
            } else {
                OutlinedTextField(
                    value = editor.text,
                    onValueChange = editor.onText,
                    label = { Text(option.title) },
                    singleLine = true,
                    enabled = option.enabled,
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = editor.keyboardOptions,
                    supportingText = if (option.supporting.isNotBlank()) {
                        { Text(option.supporting) }
                    } else {
                        null
                    },
                    modifier = editor.modifier,
                )
            }
        }

        is MenuOption.Flipper -> MenuRow(
            focused = focused,
            onClick = {
                onOptionTouched(option.id)
                option.onActivate()
            },
            enabled = option.enabled,
        ) {
            if (useLightweightTextRows) {
                TextOptionDisplay(
                    title = option.title,
                    value = if (option.checked) "On" else "Off",
                    supporting = option.subtitle,
                )
                return@MenuRow
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f).padding(end = 12.dp)) {
                    Text(option.title, style = MaterialTheme.typography.bodyLarge)
                    if (option.subtitle.isNotBlank()) {
                        Text(
                            option.subtitle,
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                // Display only: the whole row is the touch target and the focus target.
                Switch(
                    checked = option.checked,
                    onCheckedChange = null,
                    enabled = option.enabled,
                    modifier = Modifier.focusProperties { canFocus = false },
                )
            }
        }

        is MenuOption.Pill -> MenuRow(focused) {
            if (useLightweightTextRows) {
                TextOptionDisplay(
                    title = option.title,
                    value = option.choices.getOrNull(option.selectedIndex)?.label.orEmpty(),
                    supporting = "",
                )
            } else {
                Text(option.title, style = MaterialTheme.typography.titleSmall)
                PillChips(option, onOptionTouched)
            }
        }

        is MenuOption.Slider -> MenuRow(focused) {
            if (useLightweightTextRows) {
                TextOptionDisplay(
                    title = option.title,
                    value = if (option.range.endInclusive <= 1f) {
                        "${(option.value * 100f).toInt()}%"
                    } else {
                        option.value.toInt().toString()
                    },
                    supporting = "",
                )
            } else {
                Text(option.title, style = MaterialTheme.typography.titleSmall)
                Slider(
                    value = option.value,
                    onValueChange = { value ->
                        onOptionTouched(option.id)
                        option.onChange(value)
                    },
                    valueRange = option.range,
                    steps = option.steps,
                    enabled = option.enabled,
                    modifier = Modifier
                        .fillMaxWidth()
                        .focusProperties { canFocus = false },
                )
            }
        }

        is MenuOption.Action -> MenuRow(focused) {
            if (useLightweightTextRows) {
                TextOptionDisplay(title = option.title, value = "", supporting = "")
            } else {
                ActionButton(option, onOptionTouched)
            }
        }
    }
}

@Composable
private fun TextInputDisplay(
    title: String,
    value: String,
    placeholder: String,
    supporting: String,
    isError: Boolean,
) {
    Text(title, style = MaterialTheme.typography.titleSmall)
    val display = value.ifBlank { placeholder }
    if (display.isNotBlank()) {
        Text(
            display,
            style = MaterialTheme.typography.bodyMedium,
            color = when {
                isError -> MaterialTheme.colorScheme.error
                value.isBlank() -> MaterialTheme.colorScheme.onSurfaceVariant
                else -> MaterialTheme.colorScheme.onSurface
            },
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
    if (supporting.isNotBlank()) {
        Text(
            supporting,
            style = MaterialTheme.typography.bodySmall,
            color = if (isError) {
                MaterialTheme.colorScheme.error
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant
            },
        )
    }
}

@Composable
private fun TvHardwareTextField(
    optionId: String,
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    enabled: Boolean,
    modifier: Modifier,
    isError: Boolean = false,
    placeholder: String = "",
    supporting: String = "",
    keyboardOptions: KeyboardOptions = KeyboardOptions.Default,
    visualTransformation: VisualTransformation = VisualTransformation.None,
) {
    var fieldValue by remember(optionId) {
        mutableStateOf(TextFieldValue(value, selection = TextRange(value.length)))
    }
    val immediateFieldValue = remember(optionId) {
        mutableStateOf(TextFieldValue(value, selection = TextRange(value.length)))
    }
    var committedText by remember(optionId) { mutableStateOf(value) }
    var dirty by remember(optionId) { mutableStateOf(false) }
    LaunchedEffect(value) {
        if (value == committedText || dirty) return@LaunchedEffect
        committedText = value
        if (value == fieldValue.text) return@LaunchedEffect
        val next = TextFieldValue(value, selection = TextRange(value.length))
        immediateFieldValue.value = next
        fieldValue = next
    }

    fun edit(next: TextFieldValue) {
        val cleaned = next.text.lineSequence().firstOrNull().orEmpty()
        val cursor = next.selection.start.coerceIn(0, cleaned.length)
        val normalized = TextFieldValue(cleaned, selection = TextRange(cursor))
        immediateFieldValue.value = normalized
        fieldValue = normalized
        dirty = normalized.text != committedText
    }

    fun flush() {
        val text = immediateFieldValue.value.text
        if (text != committedText) {
            committedText = text
            dirty = false
            onValueChange(text)
        }
    }

    DisposableEffect(optionId) {
        onDispose { flush() }
    }

    OutlinedTextField(
        value = fieldValue,
        onValueChange = { next ->
            // Phone/tablet use the normal field path. On TV this still lets paste/selection
            // updates from Compose stay coherent if the platform ever supplies them.
            edit(next)
        },
        label = { Text(label) },
        singleLine = true,
        enabled = enabled,
        isError = isError,
        placeholder = if (placeholder.isNotBlank()) {
            { Text(placeholder) }
        } else {
            null
        },
        supportingText = if (supporting.isNotBlank()) {
            { Text(supporting) }
        } else {
            null
        },
        keyboardOptions = keyboardOptions,
        visualTransformation = visualTransformation,
        modifier = modifier.onPreviewKeyEvent { event ->
            val native = event.nativeKeyEvent
            if (native.action != AndroidKeyEvent.ACTION_DOWN) {
                return@onPreviewKeyEvent false
            }
            when (native.keyCode) {
                AndroidKeyEvent.KEYCODE_DPAD_LEFT -> {
                    val next = immediateFieldValue.value.moveCursor(-1)
                    immediateFieldValue.value = next
                    fieldValue = next
                    true
                }
                AndroidKeyEvent.KEYCODE_DPAD_RIGHT -> {
                    val next = immediateFieldValue.value.moveCursor(1)
                    immediateFieldValue.value = next
                    fieldValue = next
                    true
                }
                AndroidKeyEvent.KEYCODE_DEL -> {
                    edit(immediateFieldValue.value.deleteSelectionOrAdjacent(forward = false))
                    true
                }
                AndroidKeyEvent.KEYCODE_FORWARD_DEL -> {
                    edit(immediateFieldValue.value.deleteSelectionOrAdjacent(forward = true))
                    true
                }
                else -> {
                    val typed = printableTextChar(native.unicodeChar)
                    if (typed == null) {
                        false
                    } else {
                        edit(immediateFieldValue.value.replaceSelection(typed))
                        true
                    }
                }
            }
        },
    )
}

private fun TextFieldValue.moveCursor(delta: Int): TextFieldValue {
    val cursor = if (selection.collapsed) {
        selection.start
    } else if (delta < 0) {
        selection.min
    } else {
        selection.max
    }
    return copy(selection = TextRange((cursor + delta).coerceIn(0, text.length)))
}

private fun TextFieldValue.replaceSelection(char: Char): TextFieldValue {
    val start = selection.min.coerceIn(0, text.length)
    val end = selection.max.coerceIn(start, text.length)
    val next = text.substring(0, start) + char + text.substring(end)
    val cursor = start + 1
    return TextFieldValue(next, selection = TextRange(cursor))
}

private fun TextFieldValue.deleteSelectionOrAdjacent(forward: Boolean): TextFieldValue {
    val start = selection.min.coerceIn(0, text.length)
    val end = selection.max.coerceIn(start, text.length)
    if (start != end) {
        val next = text.removeRange(start, end)
        return TextFieldValue(next, selection = TextRange(start))
    }
    return if (forward) {
        if (start >= text.length) this else TextFieldValue(
            text.removeRange(start, start + 1),
            selection = TextRange(start),
        )
    } else {
        if (start <= 0) this else TextFieldValue(
            text.removeRange(start - 1, start),
            selection = TextRange(start - 1),
        )
    }
}

private const val DELETE_CHAR = 127

private fun printableTextChar(codePoint: Int): Char? =
    when {
        codePoint == ' '.code -> ' '
        codePoint <= ' '.code -> null
        codePoint == DELETE_CHAR -> null
        else -> codePoint.toChar()
    }

@Composable
private fun TextOptionDisplay(
    title: String,
    value: String,
    supporting: String,
) {
    Text(title, style = MaterialTheme.typography.titleSmall)
    if (value.isNotBlank()) {
        Text(
            value,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
    if (supporting.isNotBlank()) {
        Text(
            supporting,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PillChips(option: MenuOption.Pill, onOptionTouched: (String) -> Unit) {
    FlowRow(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        option.choices.forEachIndexed { index, choice ->
            FilterChip(
                selected = index == option.selectedIndex,
                onClick = {
                    onOptionTouched(option.id)
                    choice.onSelect()
                },
                label = { Text(choice.label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                enabled = option.enabled,
                modifier = Modifier.focusProperties { canFocus = false },
            )
        }
    }
}

@Composable
private fun ActionButton(option: MenuOption.Action, onOptionTouched: (String) -> Unit) {
    val onClick = {
        onOptionTouched(option.id)
        option.onRun()
    }
    val modifier = Modifier
        .fillMaxWidth()
        .focusProperties { canFocus = false }
    val label: @Composable () -> Unit = { Text(option.title) }
    when (option.style) {
        MenuOption.Action.Style.Filled -> Button(onClick, modifier, enabled = option.enabled) {
            label()
        }
        MenuOption.Action.Style.Tonal ->
            FilledTonalButton(onClick, modifier, enabled = option.enabled) { label() }
        MenuOption.Action.Style.Outlined ->
            OutlinedButton(onClick, modifier, enabled = option.enabled) { label() }
        MenuOption.Action.Style.Text ->
            TextButton(onClick, modifier, enabled = option.enabled) { label() }
    }
}

@Composable
private fun NoteRow(option: MenuOption.Note) {
    val style = when (option.style) {
        MenuOption.Note.Style.Header -> MaterialTheme.typography.titleMedium
        MenuOption.Note.Style.SubHeader -> MaterialTheme.typography.titleSmall
        MenuOption.Note.Style.Body -> MaterialTheme.typography.bodyMedium
        MenuOption.Note.Style.Small -> MaterialTheme.typography.bodySmall
    }
    val color = when (option.emphasis) {
        MenuOption.Note.Emphasis.Normal -> MaterialTheme.colorScheme.onSurface
        MenuOption.Note.Emphasis.Muted -> MaterialTheme.colorScheme.onSurfaceVariant
        MenuOption.Note.Emphasis.Primary -> MaterialTheme.colorScheme.primary
        MenuOption.Note.Emphasis.Error -> MaterialTheme.colorScheme.error
    }
    Text(
        option.title,
        style = style,
        color = color,
        modifier = Modifier.padding(horizontal = 4.dp, vertical = 2.dp),
    )
}

/**
 * Keeps the cursor on screen in either column: when the highlight lands here, ask the
 * enclosing scroll container to reveal this row.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun Modifier.revealWhenFocused(focused: Boolean): Modifier {
    val configuration = LocalConfiguration.current
    val isTv =
        configuration.uiMode and Configuration.UI_MODE_TYPE_MASK ==
            Configuration.UI_MODE_TYPE_TELEVISION
    if (isTv) return this
    val requester = remember { BringIntoViewRequester() }
    LaunchedEffect(focused) {
        if (focused) runCatching { requester.bringIntoView() }
    }
    return bringIntoViewRequester(requester)
}

/**
 * The one place cursor chrome is drawn. A filled container plus a heavy border reads at TV
 * distance, where Material's default indication does not. Shared with the Games list so
 * every column highlights the same way.
 */
@Composable
fun Modifier.cursorChrome(focused: Boolean, shape: Shape = RowShape): Modifier =
    clip(shape).then(
        if (focused) {
            Modifier
                .background(MaterialTheme.colorScheme.primaryContainer)
                .border(3.dp, SolidColor(MaterialTheme.colorScheme.primary), shape)
        } else {
            Modifier
        },
    )

@Composable
private fun MenuRow(
    focused: Boolean,
    onClick: (() -> Unit)? = null,
    enabled: Boolean = true,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .revealWhenFocused(focused)
            .cursorChrome(focused)
            .then(
                if (onClick != null) {
                    Modifier.clickable(enabled = enabled, onClick = onClick)
                } else {
                    Modifier
                },
            )
            .padding(horizontal = 8.dp, vertical = 6.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
        content = content,
    )
}

/**
 * Drawer entries. A heavy ring is the cursor; a soft fill is the pane currently on
 * screen, so both are visible at once.
 *
 * Owns its own scroll so the caller can pin chrome above it, and so a D-pad cursor that
 * walks past the bottom of the sheet pulls the list along.
 */
@Composable
fun MenuDrawerSections(
    sections: List<MenuSection>,
    focus: MenuFocus,
    currentSection: NavSection,
    onSelect: (NavSection) -> Unit,
    modifier: Modifier = Modifier,
) {
    val listState = rememberLazyListState()
    val configuration = LocalConfiguration.current
    val isTv =
        configuration.uiMode and Configuration.UI_MODE_TYPE_MASK ==
            Configuration.UI_MODE_TYPE_TELEVISION
    LaunchedEffect(focus.section, focus.inOptions, sections) {
        if (focus.inOptions) return@LaunchedEffect
        val target = sections.indexOfFirst { it.id == focus.section }
        if (target < 0) return@LaunchedEffect
        if (isTv) {
            runCatching { listState.scrollToItem(target) }
        } else {
            runCatching { listState.animateScrollToItem(target) }
        }
    }
    LazyColumn(state = listState, modifier = modifier) {
        items(sections, key = { it.id }) { section ->
            DrawerSectionRow(
                section = section,
                cursorHere = !focus.inOptions && focus.section == section.id,
                paneHere = currentSection == section.id,
                onSelect = onSelect,
            )
        }
    }
}

@Composable
private fun DrawerSectionRow(
    section: MenuSection,
    cursorHere: Boolean,
    paneHere: Boolean,
    onSelect: (NavSection) -> Unit,
) {
    val configuration = LocalConfiguration.current
    val isTv =
        configuration.uiMode and Configuration.UI_MODE_TYPE_MASK ==
            Configuration.UI_MODE_TYPE_TELEVISION
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 2.dp)
            .revealWhenFocused(cursorHere)
            .clip(DrawerRowShape)
            .then(
                when {
                    cursorHere -> Modifier
                        .background(MaterialTheme.colorScheme.primaryContainer)
                        .border(
                            3.dp,
                            SolidColor(MaterialTheme.colorScheme.primary),
                            DrawerRowShape,
                        )
                    paneHere && !isTv -> Modifier
                        .background(MaterialTheme.colorScheme.secondaryContainer)
                    else -> Modifier
                },
            )
            .clickable(enabled = section.enabled) { onSelect(section.id) }
            .padding(horizontal = 20.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            section.title,
            color = if (section.enabled) {
                MaterialTheme.colorScheme.onSurface
            } else {
                MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
            },
        )
    }
}

@Composable
fun MenuBusyIndicator() {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.Center,
    ) {
        CircularProgressIndicator()
    }
}
