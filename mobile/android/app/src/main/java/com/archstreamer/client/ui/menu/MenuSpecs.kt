package com.archstreamer.client.ui.menu

import com.archstreamer.client.net.HostAddresses
import com.archstreamer.client.protocol.MediaQualityTier
import com.archstreamer.client.protocol.MediaStreamBitrate
import com.archstreamer.client.protocol.MediaStreamFeel
import com.archstreamer.client.protocol.MediaStreamSize
import com.archstreamer.client.ui.ClientViewModel
import com.archstreamer.client.ui.NavSection
import com.archstreamer.client.ui.OverlayLayoutMode
import com.archstreamer.client.ui.OverlayProfile
import com.archstreamer.client.ui.OverlaySystemFamily
import com.archstreamer.client.ui.UiState

private fun header(id: String, text: String) =
    MenuOption.Note(id, text, MenuOption.Note.Style.Header)

private fun subHeader(id: String, text: String) =
    MenuOption.Note(id, text, MenuOption.Note.Style.SubHeader)

private fun body(id: String, text: String) =
    MenuOption.Note(id, text, MenuOption.Note.Style.Body)

private fun small(
    id: String,
    text: String,
    emphasis: MenuOption.Note.Emphasis = MenuOption.Note.Emphasis.Normal,
) = MenuOption.Note(id, text, MenuOption.Note.Style.Small, emphasis)

/** Client tab — join host / discovery. */
object ClientSpec : SectionSpec {
    override val id = NavSection.Client

    override fun isAvailable(state: UiState) = !state.playing

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        add(header("client-title", "Client Session"))
        add(
            body(
                "client-blurb",
                "Searches Wi‑Fi and VPN for a running host. If your saved IP is down, " +
                    "a live one is selected automatically.",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "client-host",
                title = "Host IP",
                value = state.client.host,
                onChange = vm::onHostChange,
                placeholder = "192.168.x.x or 10.6.0.x",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "client-alt-host",
                title = "Alt IP",
                value = state.client.altHost,
                onChange = vm::onAltHostChange,
                placeholder = "optional — e.g. WireGuard 10.6.0.x",
                supporting = "Tried when Host IP is unreachable. Must look like an IP address.",
                isError = state.client.altHost.isNotBlank() &&
                    !HostAddresses.looksLikeIp(state.client.altHost),
            ),
        )
        add(
            MenuOption.PasswordInput(
                id = "client-password",
                title = "Password (session only)",
                value = state.client.password,
                onChange = vm::onPasswordChange,
            ),
        )
        if (state.client.discoveryStatus.isNotBlank()) {
            add(small("client-discovery", state.client.discoveryStatus))
        }
        if (state.client.discoveredHosts.isNotEmpty()) {
            add(
                MenuOption.Pill(
                    id = "client-found-hosts",
                    title = "Found hosts",
                    choices = state.client.discoveredHosts.map { host ->
                        MenuOption.Pill.Choice("${host.username} @ ${host.address}") {
                            vm.selectDiscoveredHost(host)
                        }
                    },
                    selectedIndex = state.client.discoveredHosts.indexOfFirst {
                        it.address == state.client.host
                    },
                ),
            )
        }
        add(
            MenuOption.Action(
                id = "client-connect",
                title = when {
                    state.busy -> "Connecting…"
                    state.connected -> "Reconnect"
                    else -> "Connect"
                },
                onRun = vm::connect,
                enabled = !state.busy,
            ),
        )
        if (state.connected) {
            add(
                MenuOption.Action(
                    id = "client-disconnect",
                    title = "Disconnect",
                    onRun = vm::disconnect,
                    style = MenuOption.Action.Style.Text,
                ),
            )
        }
        if (state.busy) {
            add(MenuOption.Custom("client-busy") { MenuBusyIndicator() })
        }
        add(small("client-status", state.status))
    }
}

/** Remote tab — SSH ensure/stop host and presence. */
object RemoteSpec : SectionSpec {
    override val id = NavSection.Remote

    override fun isAvailable(state: UiState) =
        !state.playing || state.canAccessRemoteDuringPlay()

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        add(header("remote-title", "Remote host (SSH)"))
        add(
            body(
                "remote-blurb",
                "Ensure Host probes the base control port, reuses a free lobby, or " +
                    "SSH-starts host_runner (or an optional start script with ports + GPU). " +
                    "Optional GPU fuzzy-matches remote GPUs (host_runner --list-gpus). " +
                    "Successful ensure writes IP/ports onto the Client tab.",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-ssh-host",
                title = "SSH host",
                value = state.remote.sshHost,
                onChange = vm::onRemoteSshHostChange,
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-ssh-user",
                title = "SSH user",
                value = state.remote.sshUser,
                onChange = vm::onRemoteSshUserChange,
            ),
        )
        add(
            MenuOption.PasswordInput(
                id = "remote-ssh-password",
                title = "SSH password (not saved)",
                value = state.remote.sshPassword,
                onChange = vm::onRemoteSshPasswordChange,
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-ssh-port",
                title = "SSH port",
                value = state.remote.sshPort,
                onChange = vm::onRemoteSshPortChange,
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-directory",
                title = "Remote directory",
                value = state.remote.directory,
                onChange = vm::onRemoteDirectoryChange,
                placeholder = "/home/user/ArchStreamer/build",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-rom-root",
                title = "Remote ROM root",
                value = state.remote.romRoot,
                onChange = vm::onRemoteRomRootChange,
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-binary",
                title = "host_runner path",
                value = state.remote.binary,
                onChange = vm::onRemoteBinaryChange,
                placeholder = "./host_runner or …/build/host_runner",
                supporting = "Path A, or GPU listing. If you paste the build directory, " +
                    "/host_runner is appended. With a start script, only used for --list-gpus.",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-start-script",
                title = "Start script (optional)",
                value = state.remote.startScript,
                onChange = vm::onRemoteStartScriptChange,
                placeholder = "/home/user/bin/archstreamer-start",
                supporting = "Path B: blank = start host_runner with full args. Set = run this " +
                    "script with ports + GPU only (script owns ROM root / host_runner).",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-gpu",
                title = "GPU (optional)",
                value = state.remote.gpu,
                onChange = vm::onRemoteGpuChange,
                placeholder = "e.g. 3060, amd, nvidia:1",
                supporting = "Blank = host default. Set to reuse/start on a matched remote GPU.",
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-base-control",
                title = "Base control port",
                value = state.remote.baseControlPort,
                onChange = vm::onRemoteBaseControlPortChange,
            ),
        )
        add(
            MenuOption.TextInput(
                id = "remote-base-input",
                title = "Base input port",
                value = state.remote.baseInputPort,
                onChange = vm::onRemoteBaseInputPortChange,
            ),
        )
        add(
            MenuOption.Action(
                id = "remote-ensure",
                title = if (state.remote.busy) "Working…" else "Ensure Host",
                onRun = vm::ensureRemoteHost,
                enabled = !state.remote.busy,
            ),
        )
        add(
            MenuOption.Action(
                id = "remote-stop",
                title = "Stop Host",
                onRun = vm::stopRemoteHost,
                style = MenuOption.Action.Style.Outlined,
                enabled = !state.remote.busy,
            ),
        )
        add(subHeader("remote-users-title", "Remote users"))
        add(
            small(
                "remote-users-blurb",
                "Lists Connected/Active from the host saves root. Kick uses the same markers " +
                    "as Users. Stop Host uses the tracked port, or the GPU field to find that " +
                    "instance.",
            ),
        )
        add(
            MenuOption.Action(
                id = "remote-refresh-users",
                title = "Refresh users",
                onRun = vm::refreshRemoteUsers,
                style = MenuOption.Action.Style.Outlined,
                enabled = !state.remote.busy,
            ),
        )
        add(
            MenuOption.Action(
                id = "remote-kick",
                title = "Kick selected",
                onRun = vm::kickRemoteUser,
                style = MenuOption.Action.Style.Outlined,
                enabled = !state.remote.busy && state.remote.selectedUserIndex >= 0,
            ),
        )
        // A row per presence line rather than one list widget: the cursor has to be able to
        // land on a user, or Kick can only ever be reached by tapping and a TV cannot kick
        // anyone. Ids carry the client and slot so the highlight survives a refresh.
        if (state.remote.users.isEmpty()) {
            add(small("remote-users-empty", "No remote users loaded yet."))
        }
        state.remote.users.forEachIndexed { index, row ->
            add(
                MenuOption.Action(
                    id = "remote-user-${row.kind}-${row.clientId}-${row.slotIndex}",
                    title = row.label(),
                    onRun = { vm.selectRemoteUser(index) },
                    style = if (index == state.remote.selectedUserIndex) {
                        MenuOption.Action.Style.Filled
                    } else {
                        MenuOption.Action.Style.Tonal
                    },
                    enabled = !state.remote.busy,
                ),
            )
        }
        if (state.remote.busy) {
            add(MenuOption.Custom("remote-busy") { MenuBusyIndicator() })
        }
        add(
            MenuOption.TextInput(
                id = "remote-status",
                title = "Status / errors",
                value = state.remote.status,
                onChange = {},
                readOnly = true,
                minLines = 6,
                maxLines = 16,
            ),
        )
    }
}

/**
 * Games tab. The catalog is its own lazy list with its own input, so this section only
 * exists as a drawer entry: entering it reveals the list and hands input back to it.
 */
object GamesSpec : SectionSpec {
    override val id = NavSection.Games

    override fun isAvailable(state: UiState) = !state.playing

    override fun isEnabled(state: UiState) = state.connected

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = emptyList()
}

/** Stream tab — encode ladder and receive toggles. */
object StreamSpec : SectionSpec {
    override val id = NavSection.Stream

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        add(header("stream-title", "Client stream"))
        add(
            body(
                "stream-blurb",
                "Heartbeats tell the host which encode ladder to send. " +
                    "Mobile defaults are 30 fps @ 3.5 Mbps, 540p, Low latency.",
            ),
        )
        val tiers = listOf(
            MediaQualityTier.Low to "20",
            MediaQualityTier.Medium to "30",
            MediaQualityTier.High to "60",
        )
        add(
            MenuOption.Pill(
                id = "stream-frame-rate",
                title = "Frame rate",
                choices = tiers.map { (tier, label) ->
                    MenuOption.Pill.Choice(label) { vm.setStreamQuality(tier) }
                },
                selectedIndex = tiers.indexOfFirst { it.first == state.stream.quality },
            ),
        )
        val bitrates = listOf(
            MediaStreamBitrate.Kbps800 to "0.8",
            MediaStreamBitrate.Kbps3500 to "3.5",
            MediaStreamBitrate.Kbps8000 to "8",
            MediaStreamBitrate.Kbps12000 to "12",
            MediaStreamBitrate.Kbps25000 to "25",
        )
        add(
            MenuOption.Pill(
                id = "stream-bitrate",
                title = "Bitrate (Mbps)",
                choices = bitrates.map { (rate, label) ->
                    MenuOption.Pill.Choice(label) { vm.setStreamBitrate(rate) }
                },
                selectedIndex = bitrates.indexOfFirst { it.first == state.stream.bitrate },
            ),
        )
        val sizes = listOf(
            MediaStreamSize.P540 to "540p",
            MediaStreamSize.P720 to "720p",
            MediaStreamSize.P1080 to "1080p",
        )
        add(
            MenuOption.Pill(
                id = "stream-size",
                title = "Size",
                choices = sizes.map { (size, label) ->
                    MenuOption.Pill.Choice(label) { vm.setStreamSize(size) }
                },
                selectedIndex = sizes.indexOfFirst { it.first == state.stream.size },
            ),
        )
        val feels = listOf(
            MediaStreamFeel.LowLatency to "Low latency",
            MediaStreamFeel.Balanced to "Balanced",
            MediaStreamFeel.Smooth to "Smooth",
        )
        add(
            MenuOption.Pill(
                id = "stream-feel",
                title = "Stream feel",
                choices = feels.map { (feel, label) ->
                    MenuOption.Pill.Choice(label) { vm.setStreamFeel(feel) }
                },
                selectedIndex = feels.indexOfFirst { it.first == state.stream.feel },
            ),
        )
        add(
            small(
                "stream-feel-blurb",
                "Low latency = snappier controls; Smooth = more buffer (closer to older feel).",
            ),
        )
        add(
            MenuOption.Flipper(
                id = "stream-receive-video",
                title = "Receive video",
                checked = state.stream.receiveVideo,
                onChange = vm::setReceiveVideo,
            ),
        )
        add(
            MenuOption.Flipper(
                id = "stream-receive-audio",
                title = "Receive audio",
                checked = state.stream.receiveAudio,
                onChange = vm::setReceiveAudio,
                subtitle = "Off mutes game sound on this device (stream can stay up).",
            ),
        )
        if (state.stream.mediaHint.isNotBlank()) {
            add(small("stream-hint", state.stream.mediaHint))
        }
    }
}

/** Controls tab — overlay layout, physical pad, face swaps, sync. */
object ControlsSpec : SectionSpec {
    override val id = NavSection.Controls

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        val profile = state.controls.editingOverlayProfile
        val canEdit = state.profile.hasProfileUsername
        add(header("controls-title", "Overlay controller"))
        if (canEdit) {
            add(
                body(
                    "controls-blurb",
                    "Defaults follow the game system. Edit a family to override layout, " +
                        "face swaps, opacity, and one named custom (separate landscape / " +
                        "portrait). While editing a custom, select a control and use Action " +
                        "to remap (e.g. Select → Fast-forward, add Screen swap for DS). " +
                        "Remaps apply to the touch overlay and to a physical controller.",
                ),
            )
        } else {
            add(
                MenuOption.Note(
                    id = "controls-needs-profile",
                    title = "Set a save-profile username on the Profile tab before editing " +
                        "controls. The default “android” name is local-only and is not synced.",
                    style = MenuOption.Note.Style.Body,
                    emphasis = MenuOption.Note.Emphasis.Error,
                ),
            )
        }
        add(subHeader("controls-input-source", "Input source"))
        add(
            MenuOption.Flipper(
                id = "controls-use-physical",
                title = "Use physical controller",
                checked = state.controls.usePhysicalController,
                onChange = vm::setUsePhysicalController,
                subtitle = if (state.controls.usePhysicalController) {
                    if (state.controls.physicalPadConnected) {
                        "Active: ${state.controls.physicalPadLabel.ifBlank { "gamepad" }}. " +
                            "Home / Guide turns this menu on and off. Face swaps still apply."
                    } else {
                        "No pad connected — using touch overlay until one appears."
                    }
                } else {
                    "Touch overlay (default). Enable when using a Bluetooth / USB pad."
                },
            ),
        )
        add(
            MenuOption.Flipper(
                id = "controls-use-keyboard",
                title = "Keyboard plays games",
                checked = state.controls.useKeyboard,
                onChange = vm::setUseKeyboard,
                subtitle = if (state.controls.useKeyboard) {
                    if (state.controls.hasKeyboardActive) {
                        "Active: arrows are the D-pad, F fast-forwards, P pauses. " +
                            "Backspace opens this menu."
                    } else {
                        "On for when a keyboard appears — none is attached yet."
                    }
                } else {
                    "Typing and menus only — the game hears nothing from the keyboard. " +
                        "A TV remote is not affected."
                },
            ),
        )
        if (!canEdit) return@buildList

        if (state.playing) {
            add(
                small(
                    "controls-live-edit",
                    "Playing now — edits to ${state.controls.editingOverlayFamily.title} " +
                        "update the overlay immediately.",
                    MenuOption.Note.Emphasis.Primary,
                ),
            )
        }
        val families = OverlaySystemFamily.entries.toList()
        add(
            MenuOption.Pill(
                id = "controls-family",
                title = "System family",
                choices = families.map { family ->
                    MenuOption.Pill.Choice(family.title) { vm.setEditingOverlayFamily(family) }
                },
                selectedIndex = families.indexOf(state.controls.editingOverlayFamily),
            ),
        )
        val layouts = OverlayLayoutMode.builtins.toMutableList()
        val customName = profile.custom?.clampedName()
        if (customName != null) layouts.add(OverlayLayoutMode.Custom)
        add(
            MenuOption.Pill(
                id = "controls-layout",
                title = "Layout",
                choices = layouts.map { mode ->
                    val label = if (mode == OverlayLayoutMode.Custom) {
                        customName ?: mode.title
                    } else {
                        mode.title
                    }
                    MenuOption.Pill.Choice(label) { vm.setOverlayLayoutMode(mode) }
                },
                selectedIndex = layouts.indexOf(profile.layoutMode),
            ),
        )
        add(subHeader("controls-face-buttons", "Face buttons"))
        add(
            MenuOption.Flipper(
                id = "controls-swap-nw",
                title = "Swap NW (Y ↔ X)",
                checked = state.controls.editingMapProfile.swapNw,
                onChange = vm::setOverlaySwapNw,
            ),
        )
        add(
            MenuOption.Flipper(
                id = "controls-swap-se",
                title = "Swap SE (A ↔ B)",
                checked = state.controls.editingMapProfile.swapSe,
                onChange = vm::setOverlaySwapSe,
            ),
        )
        add(
            MenuOption.Slider(
                id = "controls-opacity",
                title = "Opacity ${(profile.clampedOpacity() * 100).toInt()}%",
                value = profile.clampedOpacity(),
                range = OverlayProfile.MIN_OPACITY..OverlayProfile.MAX_OPACITY,
                onChange = vm::setOverlayOpacity,
            ),
        )
        add(
            MenuOption.Action(
                id = "controls-edit-custom",
                title = if (profile.custom != null) {
                    "Edit custom layout"
                } else {
                    "Create custom layout"
                },
                onRun = vm::beginOverlayEdit,
                style = MenuOption.Action.Style.Tonal,
            ),
        )
        if (profile.custom != null) {
            add(
                MenuOption.Action(
                    id = "controls-remove-custom",
                    title = "Remove custom layout",
                    onRun = vm::clearOverlayCustom,
                    style = MenuOption.Action.Style.Text,
                ),
            )
        }
        add(
            MenuOption.Action(
                id = "controls-reset",
                title = "Reset ${state.controls.editingOverlayFamily.title} to defaults",
                onRun = vm::resetOverlayProfile,
                style = MenuOption.Action.Style.Text,
            ),
        )
        add(MenuOption.Divider("controls-sync-divider"))
        add(header("controls-sync-title", "Controls sync"))
        add(
            small(
                "controls-sync-blurb",
                if (!state.controls.syncReady) {
                    "Connect to a host with your password (or join a session) before syncing."
                } else {
                    "Pull or push this username's button maps and overlay profiles " +
                        "(SQL pack under the host save profile). Manual only — " +
                        "runtime remaps stay in memory after load."
                },
                MenuOption.Note.Emphasis.Muted,
            ),
        )
        add(
            MenuOption.Action(
                id = "controls-pull",
                title = "Pull from host",
                onRun = vm::pullControlsFromHost,
                style = MenuOption.Action.Style.Outlined,
                enabled = !state.busy && state.controls.syncReady,
            ),
        )
        add(
            MenuOption.Action(
                id = "controls-push",
                title = "Push to host",
                onRun = vm::pushControlsToHost,
                enabled = !state.busy && state.controls.syncReady,
            ),
        )
    }
}

/** Game Options tab — disc swapping and link cable. */
object GameOptionsSpec : SectionSpec {
    override val id = NavSection.GameOptions

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        val discs = state.gameOptions.playlistDiscs
        add(header("options-disc-title", "Disc control"))
        if (state.playing && discs.size >= 2) {
            add(
                small(
                    "options-disc-status",
                    state.gameOptions.discStatus.ifBlank {
                        "Disc ${state.gameOptions.discIndex + 1} / ${discs.size}"
                    },
                    MenuOption.Note.Emphasis.Muted,
                ),
            )
            add(
                MenuOption.Pill(
                    id = "options-disc",
                    title = "Select disc",
                    choices = discs.mapIndexed { index, label ->
                        MenuOption.Pill.Choice(label.ifBlank { "Disc ${index + 1}" }) {
                            vm.requestDiscSetIndex(index)
                        }
                    },
                    selectedIndex = state.gameOptions.discIndex,
                ),
            )
            add(
                MenuOption.Action(
                    id = "options-disc-prev",
                    title = "Previous disc",
                    onRun = vm::requestDiscPrev,
                    style = MenuOption.Action.Style.Outlined,
                ),
            )
            add(
                MenuOption.Action(
                    id = "options-disc-next",
                    title = "Next disc",
                    onRun = vm::requestDiscNext,
                    style = MenuOption.Action.Style.Outlined,
                ),
            )
        } else {
            add(
                body(
                    "options-disc-empty",
                    "Join a multi-disc session (.m3u) to swap discs here.",
                ),
            )
        }

        add(MenuOption.Divider("options-link-divider"))
        add(header("options-link-title", "Link with player"))
        if (state.playing && state.gameOptions.linkCapable) {
            add(
                small(
                    "options-link-blurb",
                    "Both players type each other's username and tap Request. " +
                        "The host matches when the requests are mutual.",
                    MenuOption.Note.Emphasis.Muted,
                ),
            )
            add(
                MenuOption.TextInput(
                    id = "options-link-peer",
                    title = "Other player's username",
                    value = state.gameOptions.linkPeerDraft,
                    onChange = vm::onLinkPeerChange,
                ),
            )
            if (state.gameOptions.linkStatus.isNotBlank()) {
                add(
                    small(
                        "options-link-status",
                        state.gameOptions.linkStatus,
                        MenuOption.Note.Emphasis.Primary,
                    ),
                )
            }
            add(
                MenuOption.Action(
                    id = "options-link-request",
                    title = "Request link",
                    onRun = vm::requestLink,
                ),
            )
            add(
                MenuOption.Action(
                    id = "options-link-cancel",
                    title = "Cancel link",
                    onRun = vm::cancelLink,
                    style = MenuOption.Action.Style.Outlined,
                ),
            )
        } else {
            add(
                body(
                    "options-link-empty",
                    if (state.playing) {
                        "This session is not link-capable."
                    } else {
                        "Join a link-capable session (GBA / DS / Switch) to request a peer."
                    },
                ),
            )
        }
        add(MenuOption.Divider("options-osk-divider"))
        add(
            small(
                "options-osk-blurb",
                "While playing: Session → Software keyboard opens the OSK even if the host " +
                    "did not detect a dialog.",
            ),
        )
    }
}

/** Profile tab — username and password changes. */
object ProfileSpec : SectionSpec {
    override val id = NavSection.Profile

    override fun isAvailable(state: UiState) = !state.playing

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        add(header("profile-title", "Identity"))
        add(
            MenuOption.TextInput(
                id = "profile-username",
                title = "Username (save profile)",
                value = state.profile.username,
                onChange = vm::onUsernameChange,
                supporting = "Required before Connect/Join — host saves are keyed by this name.",
            ),
        )
        add(
            small(
                "profile-username-blurb",
                "Stored on the phone and survives disconnect and app restart. " +
                    "Session password is on the Client tab.",
            ),
        )
        add(header("profile-password-title", "Change password"))
        if (state.client.password.isEmpty()) {
            add(
                MenuOption.PasswordInput(
                    id = "profile-current-password",
                    title = "Current password",
                    value = state.profile.changeCurrentPassword,
                    onChange = vm::onChangeCurrentPasswordChange,
                ),
            )
        }
        add(
            MenuOption.PasswordInput(
                id = "profile-new-password",
                title = "New password",
                value = state.profile.newPassword,
                onChange = vm::onNewPasswordChange,
            ),
        )
        add(
            MenuOption.PasswordInput(
                id = "profile-confirm-password",
                title = "Confirm new password",
                value = state.profile.confirmPassword,
                onChange = vm::onConfirmPasswordChange,
            ),
        )
        add(
            MenuOption.Action(
                id = "profile-change-password",
                title = "Change password on host",
                onRun = vm::changePasswordOnHost,
                enabled = !state.busy,
            ),
        )
        if (state.profile.passwordStatus.isNotBlank()) {
            add(small("profile-password-status", state.profile.passwordStatus))
        }
    }
}

/** Settings tab — ports and debug logging. */
object SettingsSpec : SectionSpec {
    override val id = NavSection.Settings

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        add(header("settings-title", "Local configuration"))
        add(
            MenuOption.TextInput(
                id = "settings-control-port",
                title = "Control port",
                value = state.settings.controlPort,
                onChange = vm::onControlPortChange,
            ),
        )
        add(
            MenuOption.TextInput(
                id = "settings-input-port",
                title = "Input port",
                value = state.settings.inputPort,
                onChange = vm::onInputPortChange,
            ),
        )
        add(
            small(
                "settings-ports-blurb",
                "Host IP and Alt IP are on the Client tab. Ports match the desktop host " +
                    "defaults (45555 / 45454).",
            ),
        )
        add(MenuOption.Divider("settings-diagnostics-divider"))
        add(header("settings-diagnostics-title", "Diagnostics"))
        add(
            MenuOption.TextInput(
                id = "settings-log-sessions",
                title = "Sessions to send",
                value = state.settings.logSessions,
                onChange = vm::onLogSessionsChange,
                supporting = "Recent app sessions from the on-device log file (1–20).",
            ),
        )
        add(
            MenuOption.Action(
                id = "settings-send-logs",
                title = if (state.busy) "Sending…" else "Send logs to host",
                onRun = vm::sendLogsToHost,
                enabled = !state.busy,
            ),
        )
        if (state.settings.logSendStatus.isNotBlank()) {
            add(small("settings-log-status", state.settings.logSendStatus))
        }
        add(MenuOption.Divider("settings-debug-divider"))
        add(header("settings-debug-title", "Debug"))
        add(
            MenuOption.Flipper(
                id = "settings-log-controls",
                title = "Log controls",
                checked = state.settings.logControls,
                onChange = vm::setLogControls,
                subtitle = if (state.settings.logControls) {
                    "On — overlay, physical pad, keyboard, and UDP pad/key changes are " +
                        "written to the on-device log (lines prefixed ctrl:). " +
                        "Use Send logs to host after a play session."
                } else {
                    "Off — no per-control spam. Enable to diagnose dead buttons / " +
                        "keyboard vs touch merge."
                },
            ),
        )
        add(
            MenuOption.Flipper(
                id = "settings-log-connections",
                title = "Log connections",
                checked = state.settings.logConnections,
                onChange = vm::setLogConnections,
                subtitle = if (state.settings.logConnections) {
                    "On — TCP connect/close, session/media bind lifecycle, and per-second " +
                        "RTP video stats (rx/lost/gaps/frames) while playing " +
                        "(lines prefixed conn:). Use Send logs to host after a play session."
                } else {
                    "Off — no connection lifecycle or RTP loss lines. Enable to diagnose " +
                        "drops, rebinds, join ordering, and green/stutter from packet loss."
                },
            ),
        )
    }
}

/**
 * Session actions. These used to live inline in the drawer sheet; as options they are
 * reachable the same way as everything else.
 */
object SessionSpec : SectionSpec {
    override val id = NavSection.Session

    override fun isAvailable(state: UiState) = state.playing || state.connected

    override fun options(state: UiState, vm: ClientViewModel): List<MenuOption> = buildList {
        if (state.playing) {
            add(header("session-title", "Session"))
            add(
                MenuOption.Flipper(
                    id = "session-pause",
                    title = "Pause",
                    checked = state.gameOptions.paused,
                    onChange = { enabled -> vm.setPaused(enabled, force = true) },
                ),
            )
            add(
                MenuOption.Flipper(
                    id = "session-fast-forward",
                    title = "Fast-forward",
                    checked = state.gameOptions.fastForward,
                    onChange = vm::setFastForward,
                ),
            )
            add(
                MenuOption.Action(
                    id = "session-edit-controls",
                    title = "Edit controls",
                    onRun = vm::beginOverlayEdit,
                    style = MenuOption.Action.Style.Tonal,
                ),
            )
            add(
                MenuOption.Action(
                    id = "session-soft-keyboard",
                    title = "Software keyboard (failsafe)",
                    onRun = vm::openManualSoftKeyboard,
                    style = MenuOption.Action.Style.Outlined,
                ),
            )
            add(
                MenuOption.Action(
                    id = "session-resync",
                    title = "Resync A/V",
                    onRun = vm::resyncAv,
                    style = MenuOption.Action.Style.Outlined,
                ),
            )
            if (state.status.contains("audio", ignoreCase = true) ||
                state.status.contains("A/V", ignoreCase = true)
            ) {
                add(small("session-status", state.status, MenuOption.Note.Emphasis.Muted))
            }
            add(
                MenuOption.Action(
                    id = "session-leave",
                    title = "Leave session",
                    onRun = vm::leavePlay,
                    style = MenuOption.Action.Style.Text,
                ),
            )
        } else {
            add(header("session-title", "Host connection"))
            add(small("session-host", "Connected · ${state.client.host}"))
        }
        add(
            MenuOption.Action(
                id = "session-disconnect",
                title = "Disconnect",
                onRun = vm::disconnect,
                style = MenuOption.Action.Style.Text,
            ),
        )
    }
}

/** Drawer order. Availability and enablement come from each spec. */
object AppMenu {
    private val specs: List<SectionSpec> = listOf(
        ClientSpec,
        RemoteSpec,
        GamesSpec,
        StreamSpec,
        ControlsSpec,
        GameOptionsSpec,
        ProfileSpec,
        SettingsSpec,
        SessionSpec,
    )

    fun sections(state: UiState, vm: ClientViewModel): List<MenuSection> =
        specs.filter { it.isAvailable(state) }.map { it.build(state, vm) }
}
