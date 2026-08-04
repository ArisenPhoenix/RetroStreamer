package com.archstreamer.client.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DrawerValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalDrawerSheet
import androidx.compose.material3.ModalNavigationDrawer
import androidx.compose.material3.NavigationDrawerItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.rememberDrawerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import android.content.res.Configuration
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.MediaQualityTier
import com.archstreamer.client.protocol.MediaStreamSize
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import android.graphics.Bitmap as AndroidBitmap

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ArchStreamerApp(viewModel: ClientViewModel) {
    val state by viewModel.state.collectAsState()
    val drawerState = rememberDrawerState(DrawerValue.Closed)
    val scope = rememberCoroutineScope()

    fun openDrawer() {
        scope.launch { drawerState.open() }
    }

    fun closeDrawer() {
        scope.launch { drawerState.close() }
    }

    // Scrim tap / swipe close bypasses closeDrawer(). Use settled currentValue — not
    // targetValue — so a flash Open→Closed (cutover / press-through) never pauses.
    // Absolute rule: drawer open → pause On (only after decoded frames); closed → Off.
    // Control editing relaxes pause (see ClientViewModel.syncMenuPause).
    LaunchedEffect(drawerState, state.playing) {
        if (!state.playing) return@LaunchedEffect
        snapshotFlow { drawerState.currentValue }
            .distinctUntilChanged()
            .collect { value ->
                when (value) {
                    DrawerValue.Open -> viewModel.onPlayMenuOpened()
                    DrawerValue.Closed -> viewModel.onPlayMenuClosed()
                }
            }
    }

    LaunchedEffect(viewModel) {
        viewModel.playMenuRequests.collect {
            openDrawer()
        }
    }

    ModalNavigationDrawer(
        drawerState = drawerState,
        gesturesEnabled = (!state.playing || drawerState.isOpen) && !state.overlayEditing,
        drawerContent = {
            AppDrawer(
                state = state,
                onSelect = { section ->
                    viewModel.selectSection(section)
                    closeDrawer()
                },
                onLeavePlay = {
                    viewModel.leavePlay()
                    closeDrawer()
                },
                onOpenSoftKeyboard = {
                    // Explicitly unpause before OSK; drawer close will also send Off (no-op).
                    viewModel.openManualSoftKeyboard()
                    closeDrawer()
                },
                onEditControls = {
                    viewModel.beginOverlayEdit()
                    closeDrawer()
                },
                onPausedChange = viewModel::setPaused,
                onFastForwardChange = viewModel::setFastForward,
                onDisconnect = {
                    viewModel.disconnect()
                    closeDrawer()
                },
                onClose = { closeDrawer() },
            )
        },
    ) {
        if (state.playing &&
            (state.section == NavSection.GameOptions ||
                state.section == NavSection.Stream ||
                state.section == NavSection.Settings)
        ) {
            Scaffold(
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
                Box(
                    modifier = Modifier
                        .padding(padding)
                        .fillMaxSize(),
                ) {
                    when (state.section) {
                        NavSection.GameOptions -> GameOptionsSection(state, viewModel)
                        NavSection.Stream -> StreamSection(state, viewModel)
                        NavSection.Settings -> SettingsSection(state, viewModel)
                        else -> Unit
                    }
                }
            }
        } else if (state.playing) {
            PlayScreen(
                state = state,
                viewModel = viewModel,
                onOpenMenu = { openDrawer() },
            )
        } else {
            Scaffold(
                topBar = {
                    TopAppBar(
                        title = { Text(state.section.title) },
                        navigationIcon = {
                            IconButton(onClick = { openDrawer() }) {
                                Icon(Icons.Filled.Menu, contentDescription = "Menu")
                            }
                        },
                    )
                },
            ) { padding ->
                Box(
                    modifier = Modifier
                        .padding(padding)
                        .fillMaxSize(),
                ) {
                    when (state.section) {
                        NavSection.Client -> ClientSection(state, viewModel)
                        NavSection.Remote -> RemoteSection(state, viewModel)
                        NavSection.Games -> GamesSection(state, viewModel)
                        NavSection.Stream -> StreamSection(state, viewModel)
                        NavSection.GameOptions -> GameOptionsSection(state, viewModel)
                        NavSection.Profile -> ProfileSection(state, viewModel)
                        NavSection.Settings -> SettingsSection(state, viewModel)
                    }
                }
            }
        }
        }

        // Offline layout editor (Game Options -> Customize). While playing, PlayScreen hosts it.
        if (state.overlayEditing && !state.playing) {
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
                        items = state.overlayItems,
                        opacity = state.overlayOpacity,
                        editing = true,
                        onItemsChange = viewModel::updateOverlayItems,
                        onDoneEditing = viewModel::finishOverlayEdit,
                    )
                }
            }
        }

        if (state.overlayEditNaming) {
            AlertDialog(
                onDismissRequest = viewModel::cancelOverlayEditNaming,
                title = { Text("Name custom layout") },
                text = {
                    OutlinedTextField(
                        value = state.overlayEditNameDraft,
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

        if (state.forcePasswordChange) {
            AlertDialog(
                onDismissRequest = viewModel::cancelForcePasswordChange,
                title = { Text("Choose a new password") },
                text = {
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("The host requires you to change the default password before joining.")
                        OutlinedTextField(
                            value = state.forcePasswordDraft,
                            onValueChange = viewModel::onForcePasswordDraftChange,
                            label = { Text("New password") },
                            singleLine = true,
                            visualTransformation = PasswordVisualTransformation(),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        OutlinedTextField(
                            value = state.forcePasswordConfirm,
                            onValueChange = viewModel::onForcePasswordConfirmChange,
                            label = { Text("Confirm new password") },
                            singleLine = true,
                            visualTransformation = PasswordVisualTransformation(),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        if (state.passwordStatus.isNotBlank()) {
                            Text(state.passwordStatus, color = MaterialTheme.colorScheme.error)
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

@Composable
private fun AppDrawer(
    state: UiState,
    onSelect: (NavSection) -> Unit,
    onLeavePlay: () -> Unit,
    onOpenSoftKeyboard: () -> Unit,
    onEditControls: () -> Unit,
    onPausedChange: (Boolean) -> Unit,
    onFastForwardChange: (Boolean) -> Unit,
    onDisconnect: () -> Unit,
    onClose: () -> Unit,
) {
    ModalDrawerSheet(modifier = Modifier.width(300.dp)) {
        Column(
            modifier = Modifier
                .fillMaxHeight()
                .verticalScroll(rememberScrollState())
                .padding(vertical = 12.dp),
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("ArchStreamer", style = MaterialTheme.typography.titleLarge)
                IconButton(onClick = onClose) {
                    Icon(Icons.Filled.Close, contentDescription = "Close menu")
                }
            }
            if (state.playing) {
                val gameName = state.selectedGame?.displayName?.takeIf { it.isNotBlank() }
                    ?: state.selectedGame?.id
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
                    "Connected · ${state.host}"
                } else {
                    "Not connected"
                },
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

            // While playing: only session actions + overlay/stream settings (no full nav).
            if (!state.playing) {
                NavSection.entries.forEach { section ->
                    val enabled = section != NavSection.Games || state.connected
                    NavigationDrawerItem(
                        label = {
                            Text(
                                section.title,
                                color = if (enabled) {
                                    MaterialTheme.colorScheme.onSurface
                                } else {
                                    MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
                                },
                            )
                        },
                        selected = state.section == section,
                        onClick = { if (enabled) onSelect(section) },
                        modifier = Modifier.padding(horizontal = 12.dp),
                    )
                }
            } else {
                NavigationDrawerItem(
                    label = { Text(NavSection.GameOptions.title) },
                    selected = false,
                    onClick = { onSelect(NavSection.GameOptions) },
                    modifier = Modifier.padding(horizontal = 12.dp),
                )
                NavigationDrawerItem(
                    label = { Text(NavSection.Stream.title) },
                    selected = false,
                    onClick = { onSelect(NavSection.Stream) },
                    modifier = Modifier.padding(horizontal = 12.dp),
                )
                NavigationDrawerItem(
                    label = { Text(NavSection.Settings.title) },
                    selected = false,
                    onClick = { onSelect(NavSection.Settings) },
                    modifier = Modifier.padding(horizontal = 12.dp),
                )
            }

            if (state.playing || state.connected) {
                HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))
            }
            if (state.playing) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 4.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Pause")
                    Switch(
                        checked = state.paused,
                        onCheckedChange = onPausedChange,
                    )
                }
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 4.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Fast-forward")
                    Switch(
                        checked = state.fastForward,
                        onCheckedChange = onFastForwardChange,
                    )
                }
                TextButton(
                    onClick = onEditControls,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp),
                ) {
                    Text("Edit controls")
                }
                TextButton(
                    onClick = onOpenSoftKeyboard,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp),
                ) {
                    Text("Software keyboard (failsafe)")
                }
                TextButton(
                    onClick = onLeavePlay,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp),
                ) {
                    Text("Leave session")
                }
            }
            if (state.connected || state.playing) {
                TextButton(
                    onClick = onDisconnect,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp),
                ) {
                    Text("Disconnect")
                }
            }
        }
    }
}

@Composable
private fun ClientSection(state: UiState, viewModel: ClientViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Client Session", style = MaterialTheme.typography.titleMedium)
        Text(
            "Searches Wi‑Fi and VPN for a running host. If your saved IP is down, a live one is selected automatically.",
            style = MaterialTheme.typography.bodyMedium,
        )
        OutlinedTextField(
            value = state.host,
            onValueChange = viewModel::onHostChange,
            label = { Text("Host IP") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("192.168.x.x or 10.6.0.x") },
        )
        OutlinedTextField(
            value = state.password,
            onValueChange = viewModel::onPasswordChange,
            label = { Text("Password (session only)") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        if (state.discoveryStatus.isNotBlank()) {
            Text(state.discoveryStatus, style = MaterialTheme.typography.bodySmall)
        }
        if (state.discoveredHosts.isNotEmpty()) {
            Text("Found hosts", style = MaterialTheme.typography.labelLarge)
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                state.discoveredHosts.forEach { host ->
                    val selected = host.address == state.host
                    FilterChip(
                        selected = selected,
                        onClick = { viewModel.selectDiscoveredHost(host) },
                        label = {
                            Text("${host.username} @ ${host.address}")
                        },
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
        }
        Button(
            onClick = viewModel::connect,
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (state.busy) "Connecting…" else if (state.connected) "Reconnect" else "Connect")
        }
        if (state.connected) {
            TextButton(onClick = viewModel::disconnect) { Text("Disconnect") }
        }
        if (state.busy) {
            CircularProgressIndicator(modifier = Modifier.align(Alignment.CenterHorizontally))
        }
        Text(state.status, style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun RemoteSection(state: UiState, viewModel: ClientViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Remote host (SSH)", style = MaterialTheme.typography.titleMedium)
        Text(
            "Ensure Host probes the base control port, reuses a free lobby, or SSH-starts host_runner. " +
                "Optional GPU fuzzy-matches remote GPUs (host_runner --list-gpus). " +
                "Successful ensure writes IP/ports onto the Client tab.",
            style = MaterialTheme.typography.bodyMedium,
        )
        OutlinedTextField(
            value = state.remoteSshHost,
            onValueChange = viewModel::onRemoteSshHostChange,
            label = { Text("SSH host") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.remoteSshUser,
            onValueChange = viewModel::onRemoteSshUserChange,
            label = { Text("SSH user") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.remoteSshPassword,
            onValueChange = viewModel::onRemoteSshPasswordChange,
            label = { Text("SSH password (not saved)") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.remoteSshPort,
            onValueChange = viewModel::onRemoteSshPortChange,
            label = { Text("SSH port") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.remoteDirectory,
            onValueChange = viewModel::onRemoteDirectoryChange,
            label = { Text("Remote directory") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("/home/user/ArchStreamer/build") },
        )
        OutlinedTextField(
            value = state.remoteRomRoot,
            onValueChange = viewModel::onRemoteRomRootChange,
            label = { Text("Remote ROM root") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.remoteBinary,
            onValueChange = viewModel::onRemoteBinaryChange,
            label = { Text("host_runner path") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("./host_runner or …/build/host_runner") },
            supportingText = {
                Text("If you paste the build directory, /host_runner is appended automatically.")
            },
        )
        OutlinedTextField(
            value = state.remoteGpu,
            onValueChange = viewModel::onRemoteGpuChange,
            label = { Text("GPU (optional)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("e.g. 3060, amd, nvidia:1") },
            supportingText = {
                Text("Blank = host default. Set to reuse/start on a matched remote GPU.")
            },
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = state.remoteBaseControlPort,
                onValueChange = viewModel::onRemoteBaseControlPortChange,
                label = { Text("Base control") },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
            OutlinedTextField(
                value = state.remoteBaseInputPort,
                onValueChange = viewModel::onRemoteBaseInputPortChange,
                label = { Text("Base input") },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = viewModel::ensureRemoteHost,
                enabled = !state.remoteBusy,
                modifier = Modifier.weight(1f),
            ) {
                Text(if (state.remoteBusy) "Working…" else "Ensure Host")
            }
            OutlinedButton(
                onClick = viewModel::stopRemoteHost,
                enabled = !state.remoteBusy,
                modifier = Modifier.weight(1f),
            ) {
                Text("Stop Host")
            }
        }
        if (state.remoteBusy) {
            CircularProgressIndicator(modifier = Modifier.align(Alignment.CenterHorizontally))
        }
        OutlinedTextField(
            value = state.remoteStatus,
            onValueChange = {},
            readOnly = true,
            label = { Text("Status / errors") },
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 140.dp, max = 240.dp),
            minLines = 6,
            maxLines = 16,
        )
    }
}

@Composable
private fun GamesSection(state: UiState, viewModel: ClientViewModel) {
    if (!state.connected) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text("Connect on the Client tab first.")
        }
        return
    }

    val filter = state.filter.trim().lowercase()
    val games = if (filter.isEmpty()) {
        state.games
    } else {
        state.games.filter {
            it.displayName.lowercase().contains(filter) ||
                it.systemName.lowercase().contains(filter) ||
                it.systemKey.lowercase().contains(filter)
        }
    }
    val byId = games.associateBy { it.id }
    val recentGames = state.recentGameIds.mapNotNull { byId[it] }
    val grouped = games
        .groupBy { it.systemName.ifBlank { it.systemKey.ifBlank { "Other" } } }
        .toList()
        .sortedBy { it.first.lowercase() }

    Column(modifier = Modifier.fillMaxSize()) {
        OutlinedTextField(
            value = state.filter,
            onValueChange = viewModel::onFilterChange,
            label = { Text("Filter") },
            singleLine = true,
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
        )
        Text(
            state.status,
            modifier = Modifier.padding(horizontal = 16.dp),
            style = MaterialTheme.typography.bodySmall,
        )
        if (state.reconnectHintGameId != null) {
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
                contentPadding = PaddingValues(vertical = 8.dp),
                modifier = Modifier.fillMaxSize(),
            ) {
                if (recentGames.isNotEmpty()) {
                    val recentExpanded = ClientViewModel.RECENT_GROUP in state.expandedSystems
                    item(key = "hdr-${ClientViewModel.RECENT_GROUP}") {
                        SystemGroupHeader(
                            title = ClientViewModel.RECENT_GROUP,
                            count = recentGames.size,
                            expanded = recentExpanded,
                            onClick = {
                                viewModel.toggleSystemExpanded(ClientViewModel.RECENT_GROUP)
                            },
                        )
                    }
                    if (recentExpanded) {
                        items(recentGames, key = { "recent-${it.id}" }) { game ->
                            GameRow(
                                game = game,
                                art = state.artByAssetKey[game.assetKey],
                                highlight = game.id == state.reconnectHintGameId,
                            ) { viewModel.startGame(game) }
                        }
                    }
                }
                grouped.forEach { (systemName, systemGames) ->
                    val expanded = systemName in state.expandedSystems
                    item(key = "hdr-$systemName") {
                        SystemGroupHeader(
                            title = systemName,
                            count = systemGames.size,
                            expanded = expanded,
                            onClick = { viewModel.toggleSystemExpanded(systemName) },
                        )
                    }
                    if (expanded) {
                        items(systemGames, key = { it.id }) { game ->
                            GameRow(
                                game = game,
                                art = state.artByAssetKey[game.assetKey],
                                highlight = game.id == state.reconnectHintGameId,
                            ) { viewModel.startGame(game) }
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
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
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
    onClick: () -> Unit,
) {
    ListItem(
        leadingContent = {
            GameArtThumb(art = art)
        },
        headlineContent = {
            Text(
                if (highlight) "Reconnect · ${game.displayName}" else game.displayName,
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
private fun StreamSection(state: UiState, viewModel: ClientViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Client stream", style = MaterialTheme.typography.titleMedium)
        Text(
            "Heartbeats tell the host which encode ladder to send. " +
                "Mobile defaults are Medium @ 540p.",
            style = MaterialTheme.typography.bodyMedium,
        )
        Text("Quality", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(
                selected = state.streamQuality == MediaQualityTier.Low,
                onClick = { viewModel.setStreamQuality(MediaQualityTier.Low) },
                label = { Text("Low") },
            )
            FilterChip(
                selected = state.streamQuality == MediaQualityTier.Medium,
                onClick = { viewModel.setStreamQuality(MediaQualityTier.Medium) },
                label = { Text("Medium") },
            )
        }
        Text("Size", style = MaterialTheme.typography.titleSmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(
                selected = state.streamSize == MediaStreamSize.P540,
                onClick = { viewModel.setStreamSize(MediaStreamSize.P540) },
                label = { Text("540p") },
            )
            FilterChip(
                selected = state.streamSize == MediaStreamSize.P720,
                onClick = { viewModel.setStreamSize(MediaStreamSize.P720) },
                label = { Text("720p") },
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Receive video")
            Switch(checked = state.receiveVideo, onCheckedChange = viewModel::setReceiveVideo)
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Receive audio")
            Switch(checked = state.receiveAudio, onCheckedChange = viewModel::setReceiveAudio)
        }
        if (state.mediaHint.isNotBlank()) {
            Text(state.mediaHint, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun GameOptionsSection(state: UiState, viewModel: ClientViewModel) {
    val profile = state.editingOverlayProfile
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Overlay controller", style = MaterialTheme.typography.titleMedium)
        Text(
            "Defaults follow the game system. Edit a family to override layout, " +
                "face swaps, opacity, and one named custom (separate landscape / portrait). " +
                "While editing a custom, select a control and use Action to remap " +
                "(e.g. Select → Fast-forward, R → R2 for DS screen swap). Remaps apply to " +
                "the touch overlay and to a physical controller.",
            style = MaterialTheme.typography.bodyMedium,
        )

        Text("Input source", style = MaterialTheme.typography.titleSmall)
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text("Use physical controller")
                Text(
                    if (state.usePhysicalController) {
                        if (state.physicalPadConnected) {
                            "Active: ${state.physicalPadLabel.ifBlank { "gamepad" }}. " +
                                "Home / Guide opens the menu. Face swaps still apply."
                        } else {
                            "No pad connected — using touch overlay until one appears."
                        }
                    } else {
                        "Touch overlay (default). Enable when using a Bluetooth / USB pad."
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = state.usePhysicalController,
                onCheckedChange = viewModel::setUsePhysicalController,
            )
        }

        if (state.playing) {
            Text(
                "Playing now — edits to ${state.editingOverlayFamily.title} update the overlay immediately.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.primary,
            )
        }

        Text("System family", style = MaterialTheme.typography.titleSmall)
        OverlaySystemFamily.entries.chunked(2).forEach { row ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                row.forEach { family ->
                    FilterChip(
                        selected = state.editingOverlayFamily == family,
                        onClick = { viewModel.setEditingOverlayFamily(family) },
                        label = { Text(family.title) },
                    )
                }
            }
        }

        Text("Layout", style = MaterialTheme.typography.titleSmall)
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            OverlayLayoutMode.builtins.chunked(3).forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    row.forEach { mode ->
                        FilterChip(
                            selected = profile.layoutMode == mode,
                            onClick = { viewModel.setOverlayLayoutMode(mode) },
                            label = { Text(mode.title) },
                        )
                    }
                }
            }
            profile.custom?.let { custom ->
                FilterChip(
                    selected = profile.layoutMode == OverlayLayoutMode.Custom,
                    onClick = { viewModel.setOverlayLayoutMode(OverlayLayoutMode.Custom) },
                    label = { Text(custom.clampedName()) },
                )
            }
        }

        Text("Face buttons", style = MaterialTheme.typography.titleSmall)
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Swap NW (Y ↔ X)")
            Switch(
                checked = state.editingMapProfile.swapNw,
                onCheckedChange = viewModel::setOverlaySwapNw,
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Swap SE (A ↔ B)")
            Switch(
                checked = state.editingMapProfile.swapSe,
                onCheckedChange = viewModel::setOverlaySwapSe,
            )
        }

        Text(
            "Opacity ${(profile.clampedOpacity() * 100).toInt()}%",
            style = MaterialTheme.typography.titleSmall,
        )
        Slider(
            value = profile.clampedOpacity(),
            onValueChange = viewModel::setOverlayOpacity,
            valueRange = OverlayProfile.MIN_OPACITY..OverlayProfile.MAX_OPACITY,
        )

        FilledTonalButton(
            onClick = viewModel::beginOverlayEdit,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(
                if (profile.custom != null) {
                    "Edit custom layout"
                } else {
                    "Create custom layout"
                },
            )
        }

        if (profile.custom != null) {
            TextButton(onClick = viewModel::clearOverlayCustom) {
                Text("Remove custom layout")
            }
        }

        TextButton(onClick = viewModel::resetOverlayProfile) {
            Text("Reset ${state.editingOverlayFamily.title} to defaults")
        }

        if (state.playing && state.playlistDiscs.size >= 2) {
            HorizontalDivider()
            Text("Disc control", style = MaterialTheme.typography.titleMedium)
            Text(
                state.discStatus.ifBlank {
                    "Disc ${state.discIndex + 1} / ${state.playlistDiscs.size}"
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text("Select disc", style = MaterialTheme.typography.titleSmall)
            state.playlistDiscs.withIndex().chunked(2).forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    row.forEach { (index, label) ->
                        FilterChip(
                            selected = state.discIndex == index,
                            onClick = { viewModel.requestDiscSetIndex(index) },
                            label = {
                                Text(
                                    label.ifBlank { "Disc ${index + 1}" },
                                    maxLines = 1,
                                )
                            },
                            modifier = Modifier.weight(1f),
                        )
                    }
                    if (row.size == 1) {
                        Spacer(modifier = Modifier.weight(1f))
                    }
                }
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                OutlinedButton(
                    onClick = viewModel::requestDiscPrev,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Previous")
                }
                OutlinedButton(
                    onClick = viewModel::requestDiscNext,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Next")
                }
            }
        }

        if (state.playing && state.linkCapable) {
            HorizontalDivider()
            Text("Link with player", style = MaterialTheme.typography.titleMedium)
            Text(
                "Both players type each other's username and tap Request. " +
                    "The host matches when the requests are mutual.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            OutlinedTextField(
                value = state.linkPeerDraft,
                onValueChange = viewModel::onLinkPeerChange,
                label = { Text("Other player's username") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            if (state.linkStatus.isNotBlank()) {
                Text(
                    state.linkStatus,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Button(
                    onClick = viewModel::requestLink,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Request link")
                }
                OutlinedButton(
                    onClick = viewModel::cancelLink,
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Cancel")
                }
            }
        }

        HorizontalDivider()
        Text(
            "While playing: menu → Software keyboard opens the OSK even if the host " +
                "did not detect a dialog.",
            style = MaterialTheme.typography.bodySmall,
        )
    }
}

@Composable
private fun ProfileSection(state: UiState, viewModel: ClientViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Identity", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = state.username,
            onValueChange = viewModel::onUsernameChange,
            label = { Text("Username (save profile)") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        Text(
            "Session password is on the Client tab. Change it here after connecting to a host.",
            style = MaterialTheme.typography.bodySmall,
        )
        Text("Change password", style = MaterialTheme.typography.titleMedium)
        if (state.password.isEmpty()) {
            OutlinedTextField(
                value = state.changeCurrentPassword,
                onValueChange = viewModel::onChangeCurrentPasswordChange,
                label = { Text("Current password") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth(),
            )
        }
        OutlinedTextField(
            value = state.newPassword,
            onValueChange = viewModel::onNewPasswordChange,
            label = { Text("New password") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        OutlinedTextField(
            value = state.confirmPassword,
            onValueChange = viewModel::onConfirmPasswordChange,
            label = { Text("Confirm new password") },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = viewModel::changePasswordOnHost,
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Change password on host")
        }
        if (state.passwordStatus.isNotBlank()) {
            Text(state.passwordStatus)
        }
    }
}

@Composable
private fun SettingsSection(state: UiState, viewModel: ClientViewModel) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Local configuration", style = MaterialTheme.typography.titleMedium)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = state.controlPort,
                onValueChange = viewModel::onControlPortChange,
                label = { Text("Control port") },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
            OutlinedTextField(
                value = state.inputPort,
                onValueChange = viewModel::onInputPortChange,
                label = { Text("Input port") },
                singleLine = true,
                modifier = Modifier.weight(1f),
            )
        }
        Text(
            "Host IP is on the Client tab. Ports match the desktop host defaults (45555 / 45454).",
            style = MaterialTheme.typography.bodySmall,
        )
        HorizontalDivider()
        Text("Diagnostics", style = MaterialTheme.typography.titleMedium)
        OutlinedTextField(
            value = state.logSessions,
            onValueChange = viewModel::onLogSessionsChange,
            label = { Text("Sessions to send") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
            supportingText = { Text("Recent app sessions from the on-device log file (1–20).") },
        )
        Button(
            onClick = viewModel::sendLogsToHost,
            enabled = !state.busy,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (state.busy) "Sending…" else "Send logs to host")
        }
        if (state.logSendStatus.isNotBlank()) {
            Text(state.logSendStatus, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@Composable
private fun PlayScreen(
    state: UiState,
    viewModel: ClientViewModel,
    onOpenMenu: () -> Unit,
) {
    val dualScreen = state.padLayout == PadLayout.DualScreen
    val configuration = LocalConfiguration.current
    val isPortrait = configuration.orientation == Configuration.ORIENTATION_PORTRAIT
    LaunchedEffect(isPortrait, state.overlayEditing, state.playing) {
        viewModel.setOverlayOrientation(isPortrait)
    }
    val unlockOrientation = dualScreen ||
        state.overlayEditing ||
        state.editingOverlayProfile.layoutMode == OverlayLayoutMode.Custom
    if (unlockOrientation) {
        UnlockSensorOrientationWhileVisible()
    } else {
        LockLandscapeWhileVisible()
    }
    KeepScreenOnWhileVisible()
    Box(modifier = Modifier.fillMaxSize()) {
        StreamVideoView(
            player = state.videoPlayer,
            modifier = Modifier.fillMaxSize(),
            dualScreen = dualScreen,
            portraitStack = isPortrait,
            emphBottom = DsTouchMapping.layoutEmphasizesBottom(state.dsScreenLayout),
        )
        // Stylus under pad chrome: overlapping face/dpad/shoulders win hit-testing.
        // Empty areas of the pad pass through so the bottom screen stays tappable.
        if (dualScreen && !state.overlayEditing) {
            DsBottomTouchOverlay(
                modifier = Modifier.fillMaxSize(),
                portraitHybridStack = isPortrait,
                layout = state.dsScreenLayout,
                onTouch = viewModel::onDsTouch,
            )
        }
        if (!state.physicalInputActive || state.overlayEditing) {
            GamepadOverlay(
                modifier = Modifier.fillMaxSize(),
                items = state.overlayItems,
                opacity = state.overlayOpacity,
                editing = state.overlayEditing,
                onState = viewModel::onPadState,
                onMenuClick = onOpenMenu,
                onFastForwardHold = viewModel::setFastForward,
                onItemsChange = viewModel::updateOverlayItems,
                onDoneEditing = viewModel::finishOverlayEdit,
            )
        } else {
            // Physical pad: Home/Guide opens the menu; keep a small fallback control.
            IconButton(
                onClick = onOpenMenu,
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(8.dp),
            ) {
                Icon(Icons.Filled.Menu, contentDescription = "Menu", tint = Color.White)
            }
        }
        state.softKeyboard?.let { request ->
            SoftKeyboardDialog(
                request = request,
                onSubmit = viewModel::submitSoftKeyboard,
                onCancel = viewModel::cancelSoftKeyboard,
            )
        }
    }
}
