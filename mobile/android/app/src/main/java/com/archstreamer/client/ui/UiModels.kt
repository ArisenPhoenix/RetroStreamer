package com.archstreamer.client.ui

import com.archstreamer.client.media.RtpVideoPlayer
import com.archstreamer.client.net.DiscoveredHost
import com.archstreamer.client.net.RemoteHost
import com.archstreamer.client.protocol.DsScreenLayout
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.LinkStatus
import com.archstreamer.client.protocol.MediaQualityTier
import com.archstreamer.client.protocol.MediaStreamBitrate
import com.archstreamer.client.protocol.MediaStreamFeel
import com.archstreamer.client.protocol.MediaStreamSize
import com.archstreamer.client.protocol.Protocol
import com.archstreamer.client.protocol.SoftKeyboardRequest
import com.archstreamer.client.ui.menu.MenuFocus
import android.graphics.Bitmap as AndroidBitmap

/** Client tab — join host / discovery. */
data class ClientState(
    val host: String = "",
    /** Optional backup IP (WireGuard, etc.); tried when Host IP is unreachable. */
    val altHost: String = "",
    /** Join password (Client tab), not profile change. */
    val password: String = "",
    /** Live LAN/VPN hosts from UDP discovery (ASDISC). */
    val discoveredHosts: List<DiscoveredHost> = emptyList(),
    /** Short discovery status for the Client tab. */
    val discoveryStatus: String = "",
)

/** Remote tab (SSH). Password is session-only, not persisted. */
data class RemoteState(
    val sshHost: String = "",
    val sshUser: String = "",
    val sshPassword: String = "",
    val sshPort: String = "22",
    val directory: String = "",
    val romRoot: String = "",
    val binary: String = "./host_runner",
    /** Optional remote start script (Path B); blank = start host_runner (Path A). */
    val startScript: String = "",
    /** Optional GPU preference for Ensure Host (fuzzy match); blank = host default. */
    val gpu: String = "",
    val baseControlPort: String = Protocol.DEFAULT_CONTROL_PORT.toString(),
    val baseInputPort: String = Protocol.DEFAULT_INPUT_PORT.toString(),
    val status: String =
        "Ensure Host probes the base port, reuses a free lobby, or SSH-starts host_runner " +
            "(or an optional start script with ports + GPU).",
    val busy: Boolean = false,
    val trackedControlPort: Int = 0,
    val users: List<RemoteHost.PresenceRow> = emptyList(),
    val selectedUserIndex: Int = -1,
)

/** Games tab — catalog, filter, recents, art. */
data class GamesState(
    val items: List<GameInfo> = emptyList(),
    /** Host catalog_offerings revision — used to skip full GameList when unchanged. */
    val catalogRevision: Long = 0L,
    /** Unfiltered offerings kept across Disconnect for revision cache hits. */
    val catalogCacheGames: List<GameInfo> = emptyList(),
    /** Per-user blocks cache (independent revision from catalog offerings). */
    val blocksRevision: Long = 0L,
    val blockedGameIds: List<String> = emptyList(),
    val filter: String = "",
    val selected: GameInfo? = null,
    /** Expanded system-name headers on the Games list (includes Recents group). */
    val expandedSystems: Set<String> = emptySet(),
    /**
     * Game ids recently started on this host (most recent first). Resolved against
     * [items] when rendering the Recents group.
     */
    val recentGameIds: List<String> = emptyList(),
    /** Boxart/grid thumbnails keyed by assetKey. */
    val artByAssetKey: Map<String, android.graphics.Bitmap> = emptyMap(),
    /** When set, Games list highlights this title — tap again to reclaim a reserved seat. */
    val reconnectHintGameId: String? = null,
    /**
     * Cursor in the Games pane, by row key. Null starts on the first group, and an unknown
     * key (the row was filtered away) falls back the same way.
     */
    val cursorKey: String? = null,
    /** The filter field holds IME focus: arrows and letters belong to it, not the list. */
    val filterEditing: Boolean = false,
)

/** Stream tab — quality / bitrate / receive toggles. */
data class StreamState(
    val mediaHint: String = "",
    val receiveVideo: Boolean = true,
    val receiveAudio: Boolean = true,
    /** Heartbeat wanted frame rate (mobile defaults 30 fps / Medium tier). */
    val quality: MediaQualityTier = MediaQualityTier.Medium,
    /** Heartbeat wanted encode bitrate (mobile defaults 3.5 Mbps). */
    val bitrate: MediaStreamBitrate = MediaStreamBitrate.Kbps3500,
    /** Heartbeat wanted encode size (mobile defaults 540p). */
    val size: MediaStreamSize = MediaStreamSize.P540,
    /** Heartbeat wanted stream feel (default Low latency = current host encode). */
    val feel: MediaStreamFeel = MediaStreamFeel.LowLatency,
)

/** Controls tab — overlay layout, physical pad, remap, sync. */
data class ControlsState(
    /** Authenticated LobbyPresence or live session — required before Pull/Push. */
    val syncReady: Boolean = false,
    val padLayout: PadLayout = PadLayout.Standard,
    val overlayOpacity: Float = OverlayProfile.DEFAULT_OPACITY,
    val swapNw: Boolean = false,
    val swapSe: Boolean = false,
    /** Resolved pad chrome for play / editor (defaults or custom). */
    val overlayItems: List<OverlayItem> = OverlayPresets.forLayout(PadLayout.Standard),
    /** Active orientation for custom resolve / editor. */
    val overlayOrientation: OverlayOrientation = OverlayOrientation.Landscape,
    /** In-play / options layout editor active. */
    val overlayEditing: Boolean = false,
    /** Name dialog after finishing the visual editor. */
    val overlayEditNaming: Boolean = false,
    /** Draft name for the single custom layout slot. */
    val overlayEditNameDraft: String = OverlayCustomLayout.DEFAULT_NAME,
    /** Draft custom being edited (both orientations). */
    val overlayEditLandscape: List<OverlayItem> = emptyList(),
    val overlayEditPortrait: List<OverlayItem> = emptyList(),
    /** Family currently edited in Controls. */
    val editingOverlayFamily: OverlaySystemFamily = OverlaySystemFamily.Standard,
    /** Snapshot of the profile under edit (mirrors prefs). */
    val editingOverlayProfile: OverlayProfile = OverlayProfile.DEFAULT,
    /**
     * User preference: physical USB/BT pad instead of the touch overlay.
     * Default is virtual unless a controller was already present on first launch.
     */
    val usePhysicalController: Boolean = false,
    /** At least one non-virtual gamepad/joystick is currently attached. */
    val physicalPadConnected: Boolean = false,
    /** Label of the first connected pad (empty when none). */
    val physicalPadLabel: String = "",
    /**
     * Effective play input: preference on, a pad is present, and not in overlay edit.
     * When true the touch overlay is hidden and pad events drive ControllerState.
     */
    val physicalInputActive: Boolean = false,
    /** Physical remap profile under edit (shared JSON document). */
    val editingMapProfile: ControllerMapProfile = ControllerMapProfile.DEFAULT,
    /**
     * A keyboard is in use, so the on-screen keyboard must stay down.
     *
     * Sticky for the life of the process: Bluetooth keyboards drop off the device list when
     * they idle and reappear on the next keypress, and a flag that followed that would let
     * the IME back in every time the keyboard dozed. Typing on one also sets it, which is
     * the strongest evidence there is.
     */
    val hasKeyboardActive: Boolean = false,
    /**
     * User preference: the keyboard plays games, the same way [usePhysicalController] says
     * the pad does. Until it is set by hand it follows detection, so a keyboard works the
     * moment it appears. Turning it off leaves the keyboard as a typing and menu device.
     */
    val useKeyboard: Boolean = false,
    /** Effective play input: preference on and a keyboard seen. */
    val keyboardInputActive: Boolean = false,
)

/** Game Options tab — pause/FF, discs, link cable. */
data class GameOptionsState(
    /** Play-menu FF latch (hold is separate; UI switch shows latch only). */
    val fastForward: Boolean = false,
    /** Explicit host pause (EmulatorControl); play-menu switch + auto menu-open pause. */
    val paused: Boolean = false,
    /** Multi-disc playlist labels from the active game (empty when single-file). */
    val playlistDiscs: List<String> = emptyList(),
    /** Host-confirmed disc index (0-based). */
    val discIndex: Int = 0,
    val discStatus: String = "",
    /** True when the active game's system supports link (GBA / NDS / Switch). */
    val linkCapable: Boolean = false,
    val linkPeerDraft: String = "",
    val linkStatus: String = "",
    val linkStatusKind: LinkStatus? = null,
)

/** Profile tab — username and password change flows. */
data class ProfileState(
    /** Save-profile username. Empty / placeholder "android" cannot edit or sync controls. */
    val username: String = "",
    /** True when [username] is a real save profile (not blank / not the android placeholder). */
    val hasProfileUsername: Boolean = false,
    val newPassword: String = "",
    val confirmPassword: String = "",
    /** Used on Profile when main password is empty (change-password current). */
    val changeCurrentPassword: String = "",
    val passwordStatus: String = "",
    /** When set, join is blocked waiting for a forced password change dialog. */
    val forcePasswordChange: Boolean = false,
    val forcePasswordDraft: String = "",
    val forcePasswordConfirm: String = "",
)

/**
 * Settings tab — ports and debug logging.
 * Most shared across tabs (ports, logging), so this is the shared bucket.
 */
data class SettingsState(
    val controlPort: String = Protocol.DEFAULT_CONTROL_PORT.toString(),
    val inputPort: String = Protocol.DEFAULT_INPUT_PORT.toString(),
    /** How many recent app sessions to include when sending logs. */
    val logSessions: String = "3",
    val logSendStatus: String = "",
    /**
     * Settings → Debug: append touch/physical/keyboard pad events and UDP sends
     * to the on-device client log (use Send logs to host to retrieve).
     */
    val logControls: Boolean = false,
    /**
     * Settings → Debug: append TCP/UDP connection open/close/lifecycle lines
     * (`conn:` prefix) to the on-device client log.
     */
    val logConnections: Boolean = false,
)

/** Device-to-device form pairing via QR code. Controls are synced through DB push/pull. */
data class PairingState(
    val receiveQr: AndroidBitmap? = null,
    val status: String = "",
)

/** Live play surface — video, OSK, DS layout (not a drawer tab). */
data class SessionState(
    val videoPlayer: RtpVideoPlayer? = null,
    /** Non-null while the host SoftKeyboard / manual OSK dialog should show. */
    val softKeyboard: SoftKeyboardRequest? = null,
    /** Host melonDS top/bottom panes (follows swap); drives DS touch hit target. */
    val dsScreenLayout: DsScreenLayout? = null,
)

/**
 * App UI state: top-level nav/session flags plus per-tab buckets.
 * Prefer `state.remote.sshHost`, `state.games.items`, `state.settings.controlPort`, etc.
 */
data class UiState(
    val section: NavSection = NavSection.Client,
    val playing: Boolean = false,
    val connected: Boolean = false,
    val busy: Boolean = false,
    val status: String = "Connect to a host on the Client tab.",
    val client: ClientState = ClientState(),
    val remote: RemoteState = RemoteState(),
    val games: GamesState = GamesState(),
    val stream: StreamState = StreamState(),
    val controls: ControlsState = ControlsState(),
    val gameOptions: GameOptionsState = GameOptionsState(),
    val profile: ProfileState = ProfileState(),
    val settings: SettingsState = SettingsState(),
    val pairing: PairingState = PairingState(),
    val session: SessionState = SessionState(),
    /** Drawer / option cursor for controller and remote navigation. */
    val menu: MenuFocus = MenuFocus(),
) {
    /**
     * Host admin: Profile username matches Remote SSH user → Remote stays available
     * while playing (kick/stop without leaving the session). Always available offline.
     */
    fun canAccessRemoteDuringPlay(): Boolean {
        val user = profile.username.trim()
        val sshUser = remote.sshUser.trim()
        return ClientViewModel.isProfileUsername(user) &&
            sshUser.isNotEmpty() &&
            user.equals(sshUser, ignoreCase = true)
    }

    /** True when a settings pane covers the live play surface (Back returns to the game). */
    fun playPaneVisible(): Boolean = playing && when (section) {
        NavSection.Controls,
        NavSection.GameOptions,
        NavSection.Stream,
        NavSection.Settings,
        NavSection.Session,
        -> true
        NavSection.Remote -> canAccessRemoteDuringPlay()
        else -> false
    }
}
