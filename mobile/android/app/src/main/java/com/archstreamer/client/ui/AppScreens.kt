package com.archstreamer.client.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.border
import androidx.compose.foundation.focusable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import android.content.res.Configuration
import androidx.compose.foundation.lazy.rememberLazyListState
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.ui.games.GamesRow
import com.archstreamer.client.ui.games.gamesCursor
import com.archstreamer.client.ui.games.gamesRows
import com.archstreamer.client.ui.menu.MenuDrawerSections
import com.archstreamer.client.ui.menu.cursorChrome
import com.archstreamer.client.ui.menu.MenuEffect
import com.archstreamer.client.ui.menu.MenuFieldFocus
import com.archstreamer.client.ui.menu.MenuOptionList
import com.archstreamer.client.ui.menu.MenuSection
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import android.graphics.Bitmap as AndroidBitmap

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
fun ArchStreamerApp(viewModel: ClientViewModel) {
    val state by viewModel.state.collectAsState()
    val drawerState = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()
    var hamburgerFocused by remember { mutableStateOf(false) }
    val hamburgerFocusRequester = remember { FocusRequester() }
    val fieldFocus = remember { MenuFieldFocus() }
    val focusManager = LocalFocusManager.current
    val keyboardController = LocalSoftwareKeyboardController.current
    // Drawer entries and their options both come from the spec list, so what is drawn and
    // what the navigator can reach are the same thing — literally the same build, shared
    // with the key handlers through the view model.
    val sections = remember(state) { viewModel.menuFor(state) }

    // A section can vanish under us — Disconnect while the Session pane is showing — so
    // fall back instead of stranding the user on an empty pane. The live play surface
    // deliberately has no section of its own.
    LaunchedEffect(sections.map { it.id }, state.section, state.playing) {
        val onPlaySurface = state.playing && !state.playPaneVisible()
        if (onPlaySurface || sections.any { it.id == state.section }) return@LaunchedEffect
        sections.firstOrNull { it.enabled }?.let { viewModel.selectSection(it.id) }
    }

    fun openDrawer() {
        hamburgerFocused = false
        viewModel.clearBackMenuChromeFocus()
        scope.launch { drawerState.open() }
    }

    fun closeDrawer() {
        hamburgerFocused = false
        viewModel.clearBackMenuChromeFocus()
        scope.launch { drawerState.close() }
    }

    // Close any nav drawer left open from Client/Games before play so we do not
    // treat a leftover Open as "pause the emulator".
    LaunchedEffect(state.playing) {
        if (state.playing && drawerState.isOpen) {
            drawerState.close()
        }
    }

    // Scrim tap / swipe close bypasses closeDrawer(). Use settled currentValue — not
    // targetValue — so a flash Open→Closed (cutover / press-through) never pauses.
    // Absolute rule: drawer open → pause On (only after decoded frames); closed → Off.
    // Control editing relaxes pause (see ClientViewModel.syncMenuPause).
    LaunchedEffect(drawerState) {
        snapshotFlow { drawerState.currentValue }
            .distinctUntilChanged()
            .collect { value ->
                hamburgerFocused = false
                when (value) {
                    DrawerValue.Open -> viewModel.onMenuDrawerOpened()
                    DrawerValue.Closed -> viewModel.onMenuDrawerClosed()
                }
            }
    }

    LaunchedEffect(viewModel) {
        viewModel.menuEffects.collect { effect ->
            when (effect) {
                MenuEffect.OpenDrawer -> openDrawer()
                MenuEffect.CloseDrawer -> closeDrawer()
                MenuEffect.FocusHamburger -> {
                    hamburgerFocused = true
                    runCatching { hamburgerFocusRequester.requestFocus() }
                }
                is MenuEffect.FocusField -> fieldFocus.request(effect.optionId)
            }
        }
    }

    // A field holds the IME only while something is being typed in — a menu option or the
    // Games filter. Clearing on the way down covers every exit, including the ones that
    // never reach the navigator: the drawer opening, a pane swap, or the field leaving the
    // composition entirely. Losing the focus owner that way does not always close the
    // keyboard, so hide it as well.
    val editingText = state.menu.editing || state.games.filterEditing
    var hadEditingText by remember { mutableStateOf(false) }
    LaunchedEffect(editingText) {
        if (editingText) {
            hadEditingText = true
        } else if (hadEditingText) {
            hadEditingText = false
            focusManager.clearFocus()
            keyboardController?.hide()
        }
    }

    // Text fields cannot raise the IME while a keyboard is attached — see
    // [WithoutSoftKeyboard] — but the system can still restore it across a rotation or a
    // process restore, so put it away if it turns up anyway.
    val imeVisible = WindowInsets.isImeVisible
    LaunchedEffect(imeVisible, state.controls.hasKeyboardActive) {
        if (imeVisible && state.controls.hasKeyboardActive) keyboardController?.hide()
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        gesturesEnabled = (!state.playing || drawerState.isOpen) && !state.controls.overlayEditing,
        drawerContent = {
            AppDrawer(
                state = state,
                sections = sections,
                hamburgerFocused = hamburgerFocused,
                onSelect = { section ->
                    viewModel.openSectionFromDrawer(section)
                    closeDrawer()
                },
                onClose = { closeDrawer() },
            )
        },
    ) {
        if (state.playPaneVisible()) {
            Scaffold(
                // The IME belongs in the same union as the system bars: edge-to-edge
                // ignores adjustResize, and padding for both separately stacks the
                // navigation bar under the keyboard, which jitters as the IME animates.
                contentWindowInsets = PaneInsets,
                topBar = {
                    TopAppBar(
                        title = { Text(state.section.title) },
                        navigationIcon = {
                            IconButton(onClick = { viewModel.returnToPlay() }) {
                                Icon(Icons.Filled.Close, contentDescription = "Back to game")
                            }
                        },
                    )
                },
            ) { padding ->
                SectionPane(
                    state = state,
                    viewModel = viewModel,
                    sections = sections,
                    fieldFocus = fieldFocus,
                    modifier = Modifier.padding(padding),
                )
            }
        } else if (state.playing) {
            PlayScreen(
                state = state,
                viewModel = viewModel,
                onOpenMenu = { openDrawer() },
                hamburgerFocusRequester = hamburgerFocusRequester,
                hamburgerFocused = hamburgerFocused,
                onHamburgerFocused = { focused ->
                    hamburgerFocused = focused
                    if (!focused && viewModel.isBackMenuChromeFocused()) {
                        viewModel.clearBackMenuChromeFocus()
                    }
                },
            )
        } else {
            Scaffold(
                contentWindowInsets = PaneInsets,
                topBar = {
                    TopAppBar(
                        title = { Text(state.section.title) },
                        navigationIcon = {
                            val interaction = remember { MutableInteractionSource() }
                            IconButton(
                                onClick = { openDrawer() },
                                modifier = Modifier
                                    .focusRequester(hamburgerFocusRequester)
                                    .onFocusChanged { focusState ->
                                        hamburgerFocused = focusState.isFocused
                                        if (!focusState.isFocused && viewModel.isBackMenuChromeFocused()) {
                                            viewModel.clearBackMenuChromeFocus()
                                        }
                                    }
                                    .focusable(interactionSource = interaction)
                                    .then(
                                        if (hamburgerFocused) {
                                            Modifier.border(
                                                width = 2.dp,
                                                color = MaterialTheme.colorScheme.primary,
                                                shape = CircleShape,
                                            )
                                        } else {
                                            Modifier
                                        },
                                    ),
                            ) {
                                Icon(Icons.Filled.Menu, contentDescription = "Menu")
                            }
                        },
                    )
                },
            ) { padding ->
                SectionPane(
                    state = state,
                    viewModel = viewModel,
                    sections = sections,
                    fieldFocus = fieldFocus,
                    modifier = Modifier.padding(padding),
                )
            }
        }
        }

        // Offline layout editor (Controls → Customize). While playing, PlayScreen hosts it.
        if (state.controls.overlayEditing && !state.playing) {
            Dialog(
                onDismissRequest = viewModel::cancelOverlayEdit,
                properties = DialogProperties(
                    usePlatformDefaultWidth = false,
                    dismissOnBackPress = true,
                    dismissOnClickOutside = false,
                ),
            ) {
                val configuration = LocalConfiguration.current
                val isPortrait = configuration.orientation == Configuration.ORIENTATION_PORTRAIT
                LaunchedEffect(isPortrait) {
                    viewModel.setOverlayOrientation(isPortrait)
                }
                UnlockSensorOrientationWhileVisible()
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(Color(0xFF0B1210)),
                ) {
                    GamepadOverlay(
                        modifier = Modifier.fillMaxSize(),
                        items = state.controls.overlayItems,
                        opacity = state.controls.overlayOpacity,
                        editing = true,
                        onItemsChange = viewModel::updateOverlayItems,
                        onDoneEditing = viewModel::finishOverlayEdit,
                    )
                }
            }
        }

        if (state.controls.overlayEditNaming) {
            AlertDialog(
                onDismissRequest = viewModel::cancelOverlayEditNaming,
                title = { Text("Name custom layout") },
                text = {
                    OutlinedTextField(
                        value = state.controls.overlayEditNameDraft,
                        onValueChange = viewModel::setOverlayEditNameDraft,
                        label = { Text("Name") },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                    )
                },
                confirmButton = {
                    TextButton(onClick = viewModel::confirmOverlayEditName) {
                        Text("Save")
                    }
                },
                dismissButton = {
                    TextButton(onClick = viewModel::cancelOverlayEditNaming) {
                        Text("Back")
                    }
                },
            )
        }

        if (state.profile.forcePasswordChange) {
            AlertDialog(
                onDismissRequest = viewModel::cancelForcePasswordChange,
                title = { Text("Choose a new password") },
                text = {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("The host requires you to change the default password before joining.")
                        OutlinedTextField(
                            value = state.profile.forcePasswordDraft,
                            onValueChange = viewModel::onForcePasswordDraftChange,
                            label = { Text("New password") },
                            singleLine = true,
                            visualTransformation = PasswordVisualTransformation(),
                            keyboardOptions = KeyboardOptions(
                                keyboardType = KeyboardType.Password,
                            ),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        OutlinedTextField(
                            value = state.profile.forcePasswordConfirm,
                            onValueChange = viewModel::onForcePasswordConfirmChange,
                            label = { Text("Confirm new password") },
                            singleLine = true,
                            visualTransformation = PasswordVisualTransformation(),
                            keyboardOptions = KeyboardOptions(
                                keyboardType = KeyboardType.Password,
                            ),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        if (state.profile.passwordStatus.isNotBlank()) {
                            Text(state.profile.passwordStatus, color = MaterialTheme.colorScheme.error)
                        }
                    }
                },
                confirmButton = {
                    TextButton(onClick = viewModel::submitForcePasswordChange) {
                        Text("Set password")
                    }
                },
                dismissButton = {
                    TextButton(onClick = viewModel::cancelForcePasswordChange) {
                        Text("Cancel")
                    }
                },
            )
        }
    }

/**
 * What a settings pane must stay clear of. A union takes the largest value per side, so
 * the keyboard replaces the navigation bar inset instead of stacking on top of it.
 */
private val PaneInsets: WindowInsets
    @Composable get() = WindowInsets.systemBars
        .union(WindowInsets.displayCutout)
        .union(WindowInsets.ime)

/**
 * Section drawer: chrome, then one row per available section. Highlighting lives in
 * [MenuDrawerSections] so the cursor and the visible pane can both be shown.
 */
@Composable
private fun AppDrawer(
    state: UiState,
    sections: List<MenuSection>,
    hamburgerFocused: Boolean = false,
    onSelect: (NavSection) -> Unit,
    onClose: () -> Unit,
) {
    ModalDrawerSheet(modifier = Modifier.width(300.dp)) {
        // Title, session and connection state stay put; only the section list scrolls,
        // so the status is still readable however far down the list the cursor is.
        Column(
            modifier = Modifier
                .fillMaxHeight()
                .padding(vertical = 12.dp),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Icon(
                        Icons.Filled.Menu,
                        contentDescription = "Menu",
                        modifier = Modifier
                            .then(
                                if (hamburgerFocused) {
                                    Modifier.border(
                                        width = 2.dp,
                                        color = MaterialTheme.colorScheme.primary,
                                        shape = CircleShape,
                                    )
                                } else {
                                    Modifier
                                },
                            )
                            .padding(4.dp),
                        tint = if (hamburgerFocused) {
                            MaterialTheme.colorScheme.primary
                        } else {
                            MaterialTheme.colorScheme.onSurface
                        },
                    )
                    Text("ArchStreamer", style = MaterialTheme.typography.titleLarge)
                }
                IconButton(onClick = onClose) {
                    Icon(Icons.Filled.Close, contentDescription = "Close menu")
                }
            }
            if (state.playing) {
                val gameName = state.games.selected?.title()?.takeIf { it.isNotBlank() }
                    ?: state.games.selected?.id
                    ?: "In session"
                Text(
                    gameName,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 2.dp),
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 2,
                )
            }
            Text(
                if (state.connected || state.playing) {
                    "Connected · ${state.client.host}"
                } else {
                    "Not connected"
                },
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
            MenuDrawerSections(
                sections = sections,
                focus = state.menu,
                currentSection = state.section,
                onSelect = onSelect,
                modifier = Modifier.weight(1f),
            )
        }
    }
}

/**
 * The options column. Every section renders from its spec; the games catalog keeps its
 * own lazy list because it owns its own input.
 */
@Composable
private fun SectionPane(
    state: UiState,
    viewModel: ClientViewModel,
    sections: List<MenuSection>,
    fieldFocus: MenuFieldFocus,
    modifier: Modifier = Modifier,
) {
    val section = sections.firstOrNull { it.id == state.section }
    // Reports how long entering a section took, composing every row included.
    LaunchedEffect(state.section) { viewModel.notePaneShown(state.section) }
    Box(modifier = modifier.fillMaxSize()) {
        when {
            state.section == NavSection.Games -> GamesSection(state, viewModel, fieldFocus)
            section == null -> Box(
                Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center,
            ) {
                Text("Not available right now.")
            }
            else -> MenuOptionList(
                section = section,
                focusedOptionId = state.menu
                    .takeIf { it.inOptions && it.section == section.id }
                    ?.optionId,
                editingOptionId = state.menu
                    .takeIf { it.editing && it.section == section.id }
                    ?.optionId,
                fieldFocus = fieldFocus,
                onOptionTouched = viewModel::onMenuOptionTouched,
                onFieldFocusChanged = viewModel::onMenuFieldFocus,
            )
        }
    }
}

/**
 * The catalog. Rows come from [gamesRows] so the cursor the view model moves and the rows
 * drawn here are the same list; the filter is pinned above and is the row above the top of
 * the list.
 */
@Composable
private fun GamesSection(
    state: UiState,
    viewModel: ClientViewModel,
    fieldFocus: MenuFieldFocus,
) {
    if (!state.connected) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("Connect on the Client tab first.")
        }
        return
    }

    val rows = gamesRows(state.games)
    val cursor = gamesCursor(rows, state.games.cursorKey)
    val listRows = rows.filterNot { it is GamesRow.Filter }
    val listState = rememberLazyListState()
    val cursorIndex = listRows.indexOfFirst { it.key == cursor?.key }
    LaunchedEffect(cursorIndex) {
        if (cursorIndex >= 0) runCatching { listState.animateScrollToItem(cursorIndex) }
    }

    Column(modifier = Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = state.games.filter,
            onValueChange = viewModel::onFilterChange,
            label = { Text("Filter") },
            singleLine = true,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .cursorChrome(cursor is GamesRow.Filter)
                .focusRequester(fieldFocus.requesterFor(GamesRow.FILTER_KEY))
                .onFocusChanged { viewModel.onGamesFilterFocus(it.isFocused) },
        )
        Text(
            state.status,
            modifier = Modifier.padding(horizontal = 16.dp),
            style = MaterialTheme.typography.bodySmall,
        )
        if (state.games.reconnectHintGameId != null) {
            Text(
                "Reconnect: tap the highlighted game (same username as before).",
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        if (state.busy) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                CircularProgressIndicator()
            }
        } else {
            LazyColumn(
                state = listState,
                contentPadding = PaddingValues(vertical = 8.dp),
                modifier = Modifier.fillMaxSize(),
            ) {
                items(listRows, key = { it.key }) { row ->
                    val onCursor = row.key == cursor?.key
                    when (row) {
                        is GamesRow.Filter -> Unit
                        is GamesRow.Header -> SystemGroupHeader(
                            title = row.system,
                            count = row.count,
                            expanded = row.expanded,
                            onCursor = onCursor,
                            onClick = {
                                viewModel.onGamesRowTouched(row.key)
                                viewModel.toggleSystemExpanded(row.system)
                            },
                        )
                        is GamesRow.Entry -> GameRow(
                            game = row.game,
                            art = state.games.artByAssetKey[row.game.assetKey],
                            highlight = row.game.id == state.games.reconnectHintGameId,
                            onCursor = onCursor,
                        ) {
                            viewModel.onGamesRowTouched(row.key)
                            viewModel.startGame(row.game)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SystemGroupHeader(
    title: String,
    count: Int,
    expanded: Boolean,
    onCursor: Boolean,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .cursorChrome(onCursor, RoundedCornerShape(12.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(
                "$count game${if (count == 1) "" else "s"}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Icon(
            imageVector = if (expanded) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore,
            contentDescription = if (expanded) "Collapse" else "Expand",
        )
    }
    HorizontalDivider()
}

@Composable
private fun GameRow(
    game: GameInfo,
    art: AndroidBitmap? = null,
    highlight: Boolean = false,
    onCursor: Boolean = false,
    onClick: () -> Unit,
) {
    ListItem(
        leadingContent = {
            GameArtThumb(art = art)
        },
        headlineContent = {
            Text(
                if (highlight) "Reconnect · ${game.title()}" else game.title(),
                color = if (highlight) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
            )
        },
        supportingContent = {
            Text(game.systemKey.ifBlank { game.systemName })
        },
        modifier = Modifier
            .fillMaxWidth()
            .cursorChrome(onCursor, RoundedCornerShape(12.dp))
            .clickable(onClick = onClick),
    )
}

@Composable
private fun GameArtThumb(art: AndroidBitmap?) {
    val shape = RoundedCornerShape(6.dp)
    Box(
        modifier = Modifier
            .size(width = 48.dp, height = 64.dp)
            .clip(shape)
            .background(MaterialTheme.colorScheme.surfaceVariant),
        contentAlignment = Alignment.Center,
    ) {
        if (art != null) {
            Image(
                bitmap = art.asImageBitmap(),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Text(
                "?",
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun PlayScreen(
    state: UiState,
    viewModel: ClientViewModel,
    onOpenMenu: () -> Unit,
    hamburgerFocusRequester: FocusRequester = FocusRequester.Default,
    hamburgerFocused: Boolean = false,
    onHamburgerFocused: (Boolean) -> Unit = {},
) {
    val dualScreen = state.controls.padLayout == PadLayout.DualScreen
    val configuration = LocalConfiguration.current
    val isPortrait = configuration.orientation == Configuration.ORIENTATION_PORTRAIT
    LaunchedEffect(isPortrait, state.controls.overlayEditing, state.playing) {
        viewModel.setOverlayOrientation(isPortrait)
    }
    val unlockOrientation = dualScreen ||
        state.controls.overlayEditing ||
        state.controls.editingOverlayProfile.layoutMode == OverlayLayoutMode.Custom
    if (unlockOrientation) {
        UnlockSensorOrientationWhileVisible()
    } else {
        LockLandscapeWhileVisible()
    }
    KeepScreenOnWhileVisible()
    Box(modifier = Modifier.fillMaxSize()) {
        StreamVideoView(
            player = state.session.videoPlayer,
            modifier = Modifier.fillMaxSize(),
            dualScreen = dualScreen,
            portraitStack = isPortrait,
            emphBottom = DsTouchMapping.layoutEmphasizesBottom(state.session.dsScreenLayout),
        )
        // Stylus under pad chrome: overlapping face/dpad/shoulders win hit-testing.
        // Empty areas of the pad pass through so the bottom screen stays tappable.
        if (dualScreen && !state.controls.overlayEditing) {
            DsBottomTouchOverlay(
                modifier = Modifier.fillMaxSize(),
                portraitHybridStack = isPortrait,
                layout = state.session.dsScreenLayout,
                onTouch = viewModel::onDsTouch,
            )
        }
        if (!state.controls.physicalInputActive || state.controls.overlayEditing) {
            GamepadOverlay(
                modifier = Modifier.fillMaxSize(),
                items = state.controls.overlayItems,
                opacity = state.controls.overlayOpacity,
                editing = state.controls.overlayEditing,
                playActionFor = viewModel::playOverlayAction,
                onState = viewModel::onPadState,
                onMenuClick = onOpenMenu,
                onFastForwardHold = viewModel::setFastForwardHold,
                onScreenSwap = viewModel::triggerScreenSwap,
                onItemsChange = viewModel::updateOverlayItems,
                onDoneEditing = viewModel::finishOverlayEdit,
            )
        }
        // Hamburger: always available for Back exit-arm focus (and as a touch fallback
        // when a physical pad hides the overlay menu control).
        if (!state.controls.overlayEditing) {
            val interaction = remember { MutableInteractionSource() }
            IconButton(
                onClick = onOpenMenu,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(8.dp)
                    .focusRequester(hamburgerFocusRequester)
                    .onFocusChanged { focusState ->
                        onHamburgerFocused(focusState.isFocused)
                    }
                    .focusable(interactionSource = interaction)
                    .then(
                        if (hamburgerFocused) {
                            Modifier.border(
                                width = 2.dp,
                                color = MaterialTheme.colorScheme.primary,
                                shape = CircleShape,
                            )
                        } else {
                            Modifier
                        },
                    ),
            ) {
                Icon(Icons.Filled.Menu, contentDescription = "Menu", tint = Color.White)
            }
        }
        state.session.softKeyboard?.let { request ->
            SoftKeyboardDialog(
                request = request,
                onSubmit = viewModel::submitSoftKeyboard,
                onCancel = viewModel::cancelSoftKeyboard,
            )
        }
    }
}
