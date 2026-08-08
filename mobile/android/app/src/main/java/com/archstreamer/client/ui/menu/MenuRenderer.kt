package com.archstreamer.client.ui.menu

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
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
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
    softKeyboard: Boolean,
    keyboardType: KeyboardType = KeyboardType.Unspecified,
): FieldEditor {
    var text by remember(optionId) { mutableStateOf(incoming) }
    var typing by remember(optionId) { mutableStateOf(false) }
    LaunchedEffect(optionId, incoming, typing) {
        if (!typing && incoming != text) text = incoming
    }
    return FieldEditor(
        text = text,
        onText = { next ->
            text = next
            onChange(next)
        },
        keyboardOptions = KeyboardOptions(
            keyboardType = keyboardType,
            showKeyboardOnFocus = softKeyboard,
        ),
        modifier = Modifier
            .fillMaxWidth()
            .focusRequester(fieldFocus.requesterFor(optionId))
            .onFocusChanged {
                typing = it.isFocused
                onFieldFocusChanged(optionId, it.isFocused)
            },
    )
}

/** Every option in a section, with one focus chrome shared by all of them. */
@Composable
fun MenuOptionList(
    section: MenuSection,
    focusedOptionId: String?,
    fieldFocus: MenuFieldFocus,
    onOptionTouched: (String) -> Unit,
    onFieldFocusChanged: (String, Boolean) -> Unit,
    /** False with a hardware keyboard attached: focusing a field must not raise the IME. */
    softKeyboard: Boolean,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 12.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        section.options.forEach { option ->
            key(option.id) {
                MenuOptionRow(
                    option = option,
                    focused = option.id == focusedOptionId,
                    fieldFocus = fieldFocus,
                    onOptionTouched = onOptionTouched,
                    onFieldFocusChanged = onFieldFocusChanged,
                    softKeyboard = softKeyboard,
                )
            }
        }
    }
}

@Composable
private fun MenuOptionRow(
    option: MenuOption,
    focused: Boolean,
    fieldFocus: MenuFieldFocus,
    onOptionTouched: (String) -> Unit,
    onFieldFocusChanged: (String, Boolean) -> Unit,
    softKeyboard: Boolean,
) {
    when (option) {
        is MenuOption.Note -> NoteRow(option)
        is MenuOption.Divider -> HorizontalDivider(modifier = Modifier.padding(vertical = 4.dp))
        is MenuOption.Custom -> option.content()

        is MenuOption.TextInput -> MenuRow(focused) {
            val editor = rememberFieldEditor(
                optionId = option.id,
                incoming = option.value,
                onChange = option.onChange,
                fieldFocus = fieldFocus,
                onFieldFocusChanged = onFieldFocusChanged,
                softKeyboard = softKeyboard,
            )
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

        is MenuOption.PasswordInput -> MenuRow(focused) {
            val editor = rememberFieldEditor(
                optionId = option.id,
                incoming = option.value,
                onChange = option.onChange,
                fieldFocus = fieldFocus,
                onFieldFocusChanged = onFieldFocusChanged,
                softKeyboard = softKeyboard,
                // Password variation: no suggestion strip, no last-character preview,
                // and the IME does not learn what was typed.
                keyboardType = KeyboardType.Password,
            )
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

        is MenuOption.Flipper -> MenuRow(
            focused = focused,
            onClick = {
                onOptionTouched(option.id)
                option.onActivate()
            },
            enabled = option.enabled,
        ) {
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
            Text(option.title, style = MaterialTheme.typography.titleSmall)
            PillChips(option, onOptionTouched)
        }

        is MenuOption.Slider -> MenuRow(focused) {
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

        is MenuOption.Action -> MenuRow(focused) {
            ActionButton(option, onOptionTouched)
        }
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
    Column(modifier = modifier.verticalScroll(rememberScrollState())) {
        sections.forEach { section ->
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
                    paneHere -> Modifier
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

/** Remote presence rows. Touch-only selection; Kick selected acts on the highlight. */
@Composable
fun RemoteUserList(state: UiState, vm: ClientViewModel) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .heightIn(max = 220.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        if (state.remote.users.isEmpty()) {
            Text("No remote users loaded yet.", style = MaterialTheme.typography.bodySmall)
            return@Column
        }
        state.remote.users.forEachIndexed { index, row ->
            val selected = index == state.remote.selectedUserIndex
            Text(
                text = row.label(),
                modifier = Modifier
                    .fillMaxWidth()
                    .background(
                        if (selected) {
                            MaterialTheme.colorScheme.primaryContainer
                        } else {
                            MaterialTheme.colorScheme.surfaceVariant
                        },
                        RoundedCornerShape(8.dp),
                    )
                    .clickable { vm.selectRemoteUser(index) }
                    .padding(horizontal = 12.dp, vertical = 8.dp),
                style = MaterialTheme.typography.bodyMedium,
            )
        }
    }
}
