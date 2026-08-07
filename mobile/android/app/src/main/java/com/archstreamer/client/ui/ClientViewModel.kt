package com.archstreamer.client.ui

import android.app.Application
import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.archstreamer.client.SessionKeepAliveService
import com.archstreamer.client.cadence.CadenceControlsStore
import com.archstreamer.client.media.RtpVideoPlayer
import com.archstreamer.client.net.ArtFetcher
import com.archstreamer.client.net.CatalogFetcher
import com.archstreamer.client.net.ClientFileLog
import com.archstreamer.client.net.ControlConnection
import com.archstreamer.client.net.DiscoveredHost
import com.archstreamer.client.net.HostAddresses
import com.archstreamer.client.net.HostDiscovery
import com.archstreamer.client.net.JoinedPlaySession
import com.archstreamer.client.net.RemoteHost
import com.archstreamer.client.net.RemoteHostPortBlock
import com.archstreamer.client.net.SessionJoiner
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import com.archstreamer.client.protocol.ControllerState
import com.archstreamer.client.protocol.DisplayLayoutPreference
import com.archstreamer.client.protocol.DiscControlAction
import com.archstreamer.client.protocol.DsScreenLayout
import com.archstreamer.client.protocol.EmulatorControlAction
import com.archstreamer.client.protocol.EmulatorControlState
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.LinkAction
import com.archstreamer.client.protocol.LinkStatus
import com.archstreamer.client.protocol.MediaQualityTier
import com.archstreamer.client.protocol.MediaStreamFeel
import com.archstreamer.client.protocol.MediaStreamSize
import com.archstreamer.client.protocol.Protocol
import com.archstreamer.client.protocol.SoftKeyboardRequest
import com.archstreamer.client.protocol.SoftKeyboardResponse
import com.archstreamer.client.protocol.systemSupportsLink
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/** Mirrors the desktop GUI top-level tabs (client-only subset). */
enum class NavSection(val title: String) {
    Client("Client"),
    Remote("Remote"),
    Games("Games"),
    Stream("Stream"),
    Controls("Controls"),
    GameOptions("Game Options"),
    Profile("Profile"),
    Settings("Settings"),
}

data class UiState(
    val section: NavSection = NavSection.Client,
    val playing: Boolean = false,
    val connected: Boolean = false,
    val host: String = "",
    /** Optional backup IP (WireGuard, etc.); tried when Host IP is unreachable. */
    val altHost: String = "",
    val controlPort: String = Protocol.DEFAULT_CONTROL_PORT.toString(),
    val inputPort: String = Protocol.DEFAULT_INPUT_PORT.toString(),
    /** Save-profile username. Empty / placeholder "android" cannot edit or sync controls. */
    val username: String = "",
    /** True when [username] is a real save profile (not blank / not the android placeholder). */
    val hasProfileUsername: Boolean = false,
    /** Authenticated LobbyPresence or live session — required before Pull/Push. */
    val controlsSyncReady: Boolean = false,
    val password: String = "",
    val newPassword: String = "",
    val confirmPassword: String = "",
    /** Used on Profile when main password is empty (change-password current). */
    val changeCurrentPassword: String = "",
    val passwordStatus: String = "",
    /** When set, join is blocked waiting for a forced password change dialog. */
    val forcePasswordChange: Boolean = false,
    val forcePasswordDraft: String = "",
    val forcePasswordConfirm: String = "",
    val busy: Boolean = false,
    val status: String = "Connect to a host on the Client tab.",
    val games: List<GameInfo> = emptyList(),
    /** Host catalog_offerings revision — used to skip full GameList when unchanged. */
    val catalogRevision: Long = 0L,
    /** Unfiltered offerings kept across Disconnect for revision cache hits. */
    val catalogCacheGames: List<GameInfo> = emptyList(),
    /** Per-user blocks cache (independent revision from catalog offerings). */
    val blocksRevision: Long = 0L,
    val blockedGameIds: List<String> = emptyList(),
    val filter: String = "",
    val selectedGame: GameInfo? = null,
    val mediaHint: String = "",
    val videoPlayer: RtpVideoPlayer? = null,
    val receiveVideo: Boolean = true,
    val receiveAudio: Boolean = true,
    /** Non-null while the host SoftKeyboard / manual OSK dialog should show. */
    val softKeyboard: SoftKeyboardRequest? = null,
    /** Host melonDS top/bottom panes (follows swap); drives DS touch hit target. */
    val dsScreenLayout: DsScreenLayout? = null,
    /** Heartbeat wanted quality (mobile defaults Medium). */
    val streamQuality: MediaQualityTier = MediaQualityTier.Medium,
    /** Heartbeat wanted encode size (mobile defaults 540p). */
    val streamSize: MediaStreamSize = MediaStreamSize.P540,
    /** Heartbeat wanted stream feel (default Low latency = current host encode). */
    val streamFeel: MediaStreamFeel = MediaStreamFeel.LowLatency,
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
    /** Expanded system-name headers on the Games list (includes [RECENT_GROUP]). */
    val expandedSystems: Set<String> = emptySet(),
    /**
     * Game ids recently started on this host (most recent first). Resolved against
     * [games] when rendering the Recents group.
     */
    val recentGameIds: List<String> = emptyList(),
    /** Boxart/grid thumbnails keyed by assetKey. */
    val artByAssetKey: Map<String, android.graphics.Bitmap> = emptyMap(),
    /** When set, Games list highlights this title — tap again to reclaim a reserved seat. */
    val reconnectHintGameId: String? = null,
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
     * When true the touch overlay is hidden and pad events drive [ControllerState].
     */
    val physicalInputActive: Boolean = false,
    /** Physical remap profile under edit (shared JSON document). */
    val editingMapProfile: ControllerMapProfile = ControllerMapProfile.DEFAULT,
    /** Live LAN/VPN hosts from UDP discovery (ASDISC). */
    val discoveredHosts: List<DiscoveredHost> = emptyList(),
    /** Short discovery status for the Client tab. */
    val discoveryStatus: String = "",
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
    // Remote tab (SSH) — password is session-only, not persisted.
    val remoteSshHost: String = "",
    val remoteSshUser: String = "",
    val remoteSshPassword: String = "",
    val remoteSshPort: String = "22",
    val remoteDirectory: String = "",
    val remoteRomRoot: String = "",
    val remoteBinary: String = "./host_runner",
    /** Optional remote start script (Path B); blank = start host_runner (Path A). */
    val remoteStartScript: String = "",
    /** Optional GPU preference for Ensure Host (fuzzy match); blank = host default. */
    val remoteGpu: String = "",
    val remoteBaseControlPort: String = Protocol.DEFAULT_CONTROL_PORT.toString(),
    val remoteBaseInputPort: String = Protocol.DEFAULT_INPUT_PORT.toString(),
    val remoteStatus: String =
        "Ensure Host probes the base port, reuses a free lobby, or SSH-starts host_runner " +
            "(or an optional start script with ports + GPU).",
    val remoteBusy: Boolean = false,
    val remoteTrackedControlPort: Int = 0,
    val remoteUsers: List<RemoteHost.PresenceRow> = emptyList(),
    val remoteSelectedUserIndex: Int = -1,
) {
    /**
     * Host admin: Profile username matches Remote SSH user → Remote stays available
     * while playing (kick/stop without leaving the session). Always available offline.
     */
    fun canAccessRemoteDuringPlay(): Boolean {
        val profile = username.trim()
        val sshUser = remoteSshUser.trim()
        return ClientViewModel.isProfileUsername(profile) &&
            sshUser.isNotEmpty() &&
            profile.equals(sshUser, ignoreCase = true)
    }
}

class ClientViewModel(application: Application) : AndroidViewModel(application) {
    private val prefs = application.getSharedPreferences(PREFS_NAME, Application.MODE_PRIVATE)
    private val cadenceControls = CadenceControlsStore(application)
    private val buttonMapFile =
        File(application.filesDir, ControllerMapDocument.FILE_NAME)
    private var overlayProfiles = loadOverlayProfiles().toMutableMap()
    private var buttonMapDocument = loadButtonMapDocument()
    private var passwordChangeLatch: java.util.concurrent.CountDownLatch? = null
    @Volatile private var passwordChangeResult: String = ""

    private val _state = MutableStateFlow(
        UiState(
            host = prefs.getString(KEY_HOST, "").orEmpty(),
            altHost = prefs.getString(KEY_ALT_HOST, "").orEmpty(),
            controlPort = prefs.getString(KEY_CONTROL_PORT, Protocol.DEFAULT_CONTROL_PORT.toString())
                .orEmpty()
                .ifBlank { Protocol.DEFAULT_CONTROL_PORT.toString() },
            inputPort = prefs.getString(KEY_INPUT_PORT, Protocol.DEFAULT_INPUT_PORT.toString())
                .orEmpty()
                .ifBlank { Protocol.DEFAULT_INPUT_PORT.toString() },
            username = prefs.getString(KEY_USERNAME, "").orEmpty().let { raw ->
                if (raw.equals(PLACEHOLDER_USERNAME, ignoreCase = true)) {
                    prefs.edit().remove(KEY_USERNAME).apply()
                    ""
                } else {
                    raw
                }
            },
            hasProfileUsername = isProfileUsername(
                prefs.getString(KEY_USERNAME, "").orEmpty(),
            ),
            streamQuality = qualityFromPrefs(),
            streamSize = sizeFromPrefs(),
            streamFeel = feelFromPrefs(),
            logSessions = prefs.getString(KEY_LOG_SESSIONS, "3").orEmpty().ifBlank { "3" },
            logControls = prefs.getBoolean(KEY_LOG_CONTROLS, false),
            logConnections = prefs.getBoolean(KEY_LOG_CONNECTIONS, false),
            editingOverlayFamily = OverlaySystemFamily.Standard,
            editingOverlayProfile = overlayProfiles[OverlaySystemFamily.Standard]
                ?: OverlayProfile.DEFAULT,
            editingMapProfile = buttonMapDocument.profile(ControllerMapFamily.Standard),
            recentGameIds = loadRecentGameIds(
                prefs.getString(KEY_HOST, "").orEmpty(),
                prefs.getString(KEY_CONTROL_PORT, Protocol.DEFAULT_CONTROL_PORT.toString())
                    .orEmpty()
                    .ifBlank { Protocol.DEFAULT_CONTROL_PORT.toString() },
            ),
            usePhysicalController = resolveInitialPreferPhysical(),
            remoteSshHost = prefs.getString(KEY_REMOTE_SSH_HOST, "").orEmpty(),
            remoteSshUser = prefs.getString(KEY_REMOTE_SSH_USER, "").orEmpty(),
            remoteSshPort = prefs.getString(KEY_REMOTE_SSH_PORT, "22").orEmpty().ifBlank { "22" },
            remoteDirectory = prefs.getString(KEY_REMOTE_DIRECTORY, "").orEmpty(),
            remoteRomRoot = prefs.getString(KEY_REMOTE_ROM_ROOT, "").orEmpty(),
            remoteBinary = prefs.getString(KEY_REMOTE_BINARY, "./host_runner").orEmpty()
                .ifBlank { "./host_runner" },
            remoteStartScript = prefs.getString(KEY_REMOTE_START_SCRIPT, "").orEmpty(),
            remoteGpu = prefs.getString(KEY_REMOTE_GPU, "").orEmpty(),
            remoteBaseControlPort = prefs.getString(
                KEY_REMOTE_BASE_CONTROL,
                Protocol.DEFAULT_CONTROL_PORT.toString(),
            ).orEmpty().ifBlank { Protocol.DEFAULT_CONTROL_PORT.toString() },
            remoteBaseInputPort = prefs.getString(
                KEY_REMOTE_BASE_INPUT,
                Protocol.DEFAULT_INPUT_PORT.toString(),
            ).orEmpty().ifBlank { Protocol.DEFAULT_INPUT_PORT.toString() },
            remoteTrackedControlPort = prefs.getInt(KEY_REMOTE_TRACKED_CONTROL, 0),
        ).let { base ->
            val pads = PhysicalGamepad.connectedPads()
            val connected = pads.isNotEmpty()
            base.copy(
                physicalPadConnected = connected,
                physicalPadLabel = pads.firstOrNull()?.name.orEmpty(),
                physicalInputActive = base.usePhysicalController && connected && !base.overlayEditing,
            )
        },
    )
    val state: StateFlow<UiState> = _state.asStateFlow()

    private val _playMenuRequests = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    /** Home / Guide on a physical pad → open the play drawer. */
    val playMenuRequests: SharedFlow<Unit> = _playMenuRequests.asSharedFlow()

    private val _playMenuCommands =
        MutableSharedFlow<PlayMenuCommand>(extraBufferCapacity = 8)
    /** D-pad / Enter / Esc while the play drawer is open. */
    val playMenuCommands: SharedFlow<PlayMenuCommand> = _playMenuCommands.asSharedFlow()

    private var session: JoinedPlaySession? = null
    /** Open control TCP after Connect (Users-tab Connected) until play/disconnect. */
    private var lobbyPresence: ControlConnection? = null
    /** In-flight ControlsDb reply while playing (session control loop delivers it). */
    private val controlsSyncWaiter =
        AtomicReference<CompletableDeferred<IncomingPacket>?>(null)
    private var inputJob: Job? = null
    private var heartbeatJob: Job? = null
    private var controlJob: Job? = null
    private var artJob: Job? = null
    private var discoveryJob: Job? = null
    private var menuPauseJob: Job? = null
    private var ffJob: Job? = null
    /** Last effective FF On/Off sent to the host (latch ∨ hold). */
    private var lastSentFf: Boolean? = null
    private var lastFfSendAtMs: Long = 0L
    /** Play-menu / switch latch — stays until the user turns it off. */
    private var ffMenuLatched = false
    /** Keyboard F / overlay / remapped pad hold. */
    private var ffHoldPressed = false
    /**
     * False until the first decoded video frame. Ignores hold edges during stream
     * init so startup noise cannot poke Ryujinx F1 before client/host agree on Off.
     */
    private var ffInputArmed = false
    private var lastAvResyncAtMs: Long = 0L
    /** Desktop client_app: ≥3 zero-frame heartbeats → realign audio when frames return. */
    private var zeroFrameStreak = 0
    private var audioRealignAfterVideoStall = false
    /** Ignore pre-first-frame zeros (stream still starting). */
    private var everDecodedVideoFrames = false
    private var latestPad = ControllerState()
    /** Last overlay/physical pad logged while [UiState.logControls] is on (dedupe spam). */
    private var lastLoggedSourcePad: ControllerState? = null
    /** Held RemotedKey bits (desktop remoted keyboard subset). */
    private val remotedKeysHeld = AtomicInteger(0)
    /** Keyboard arrow keys merged into the pad as D-pad while playing. */
    private val keyboardDpadBits = AtomicInteger(0)
    private var menuHatUp = false
    private var menuHatDown = false
    /** Stylus samples from the UI thread; drained on the input IO loop (avoid NetworkOnMainThread). */
    private val pendingDsTouches =
        java.util.concurrent.ConcurrentLinkedQueue<Triple<Int, Int, Boolean>>()
    private val gamepadTracker = PhysicalGamepadTracker(
        actionFor = { kind -> overlayActionFor(kind) },
        onState = { pad -> onPhysicalPadState(pad) },
        onMenuClick = {
            if (menuDrawerOpen) {
                _playMenuCommands.tryEmit(PlayMenuCommand.Close)
            } else {
                requestPlayMenu()
            }
        },
        onFastForward = { held -> setFastForwardHold(held) },
        onScreenSwap = { triggerScreenSwap() },
    )
    private val inputManager = application.getSystemService(InputManager::class.java)
    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) = refreshPhysicalPads()
        override fun onInputDeviceRemoved(deviceId: Int) = refreshPhysicalPads()
        override fun onInputDeviceChanged(deviceId: Int) = refreshPhysicalPads()
    }
    /**
     * Play-drawer open state from the UI. Pause is absolute:
     * wantPaused = menuDrawerOpen && !overlayEditing
     * (control editing is the only time we relax pause so the game stays live).
     */
    private var menuDrawerOpen = false
    /** Last pause On/Off actually sent — dedupe only, never used to invert. */
    private var lastSentMenuPause: Boolean? = null
    /** Live session system key — used to resolve Auto layout and apply live edits. */
    private var sessionSystemKey: String? = null
    private var sessionFamily: OverlaySystemFamily = OverlaySystemFamily.Standard
    /** Skip auto-apply briefly after the user edits the Host IP field. */
    private var hostEditSuppressUntilMs: Long = 0L
    private var discoveryAttempts: Int = 0

    init {
        ClientFileLog.init(application)
        ClientFileLog.logConnections = prefs.getBoolean(KEY_LOG_CONNECTIONS, false)
        inputManager?.registerInputDeviceListener(
            inputDeviceListener,
            Handler(Looper.getMainLooper()),
        )
        startHostDiscovery()
        viewModelScope.launch {
            var wasPlaying = false
            state.collect { snap ->
                if (snap.playing == wasPlaying) return@collect
                wasPlaying = snap.playing
                val app = getApplication<Application>()
                if (snap.playing) {
                    val title = snap.selectedGame?.title()
                        ?: snap.status.removePrefix("Playing ").takeIf { it.isNotBlank() }
                    SessionKeepAliveService.start(app, title)
                } else {
                    SessionKeepAliveService.stop(app)
                }
            }
        }
    }

    private fun profileFor(family: OverlaySystemFamily): OverlayProfile =
        overlayProfiles[family] ?: OverlayProfile.DEFAULT

    private fun mapProfileFor(family: OverlaySystemFamily): ControllerMapProfile =
        buttonMapDocument.profile(OverlaySystemFamily.toMapFamily(family))

    /**
     * Real save-profile username from SharedPreferences.
     * Rejects blank and the legacy "android" placeholder (never a valid save profile).
     */
    private fun profileUsernameOrNull(): String? {
        val raw = prefs.getString(KEY_USERNAME, "").orEmpty().trim()
        return raw.takeIf { isProfileUsername(it) }
    }

    private fun requireProfileUsername(): String =
        profileUsernameOrNull()
            ?: error("Set a save-profile username on the Profile tab first")

    private fun loadOverlayProfiles(): Map<OverlaySystemFamily, OverlayProfile> {
        val username = profileUsernameOrNull()
        if (username != null) {
            cadenceControls.findControls(username, CadenceControlsStore.KIND_OVERLAY_PROFILES)
                ?.let { json ->
                    OverlayProfileStore.decodeDocument(json)?.let { return it }
                }
            // One-shot import from SharedPreferences → SQL for this profile.
            val fromPrefs = OverlayProfileStore.loadAll(prefs)
            persistOverlayProfiles(fromPrefs, username)
            return fromPrefs
        }
        return OverlayProfileStore.loadAll(prefs)
    }

    private fun persistOverlayProfiles(
        profiles: Map<OverlaySystemFamily, OverlayProfile> = overlayProfiles,
        username: String? = profileUsernameOrNull(),
    ) {
        OverlayProfileStore.saveAll(prefs, profiles)
        if (username == null) return
        val json = OverlayProfileStore.encodeDocument(profiles)
        cadenceControls.upsertControls(
            username = username,
            kind = CadenceControlsStore.KIND_OVERLAY_PROFILES,
            documentJson = json,
            version = OverlayProfileStore.DOCUMENT_VERSION,
        )
    }

    private fun reloadControlsFromLocalStore() {
        overlayProfiles = loadOverlayProfiles().toMutableMap()
        buttonMapDocument = loadButtonMapDocument()
        val family = _state.value.editingOverlayFamily
        _state.update {
            it.copy(
                editingOverlayProfile = overlayProfiles[family] ?: OverlayProfile.DEFAULT,
                editingMapProfile = buttonMapDocument.profile(
                    OverlaySystemFamily.toMapFamily(family),
                ),
            )
        }
    }

    private fun loadButtonMapDocument(): ControllerMapDocument {
        val username = profileUsernameOrNull()
        if (username != null) {
            cadenceControls.findControls(username)?.let { json ->
                return runCatching { ControllerMapStore.decode(json) }
                    .getOrDefault(ControllerMapDocument())
            }

            // One-shot import from legacy filesDir JSON, then stop relying on the file.
            if (buttonMapFile.isFile) {
                val loaded = ControllerMapStore.load(buttonMapFile)
                cadenceControls.upsertControls(
                    username = username,
                    documentJson = ControllerMapStore.encode(loaded),
                    version = loaded.version,
                )
                return loaded
            }

            // First run: migrate overlay face swaps into the portable document.
            var doc = ControllerMapDocument()
            var changed = false
            OverlaySystemFamily.entries.forEach { family ->
                val overlay = overlayProfiles[family] ?: return@forEach
                if (!overlay.swapNw && !overlay.swapSe) return@forEach
                val mapFamily = OverlaySystemFamily.toMapFamily(family)
                val profile = doc.profile(mapFamily).copy(
                    swapNw = overlay.swapNw,
                    swapSe = overlay.swapSe,
                )
                doc = doc.withProfile(mapFamily, profile)
                changed = true
            }
            if (changed) {
                persistButtonMapDocument(doc, username)
            }
            return doc
        }

        if (buttonMapFile.isFile) {
            return ControllerMapStore.load(buttonMapFile)
        }
        return ControllerMapDocument()
    }

    private fun persistButtonMapDocument(
        document: ControllerMapDocument = buttonMapDocument,
        username: String? = profileUsernameOrNull(),
    ) {
        if (username == null) return
        cadenceControls.upsertControls(
            username = username,
            documentJson = ControllerMapStore.encode(document),
            version = document.version,
        )
    }

    /**
     * When a custom overlay is saved, mirror remappable control Actions into the shared
     * button-map document (cadence user_controls) so desktop can reuse the same remaps.
     */
    private fun syncButtonMapRemapsFromItems(family: OverlaySystemFamily, items: List<OverlayItem>) {
        fun storedAction(kind: OverlayControlKind): ControllerMapAction {
            val item = items.firstOrNull { it.kind == kind } ?: return ControllerMapAction.Default
            if (item.action == OverlayAction.Default || item.resolvedAction() == kind.defaultAction) {
                return ControllerMapAction.Default
            }
            return overlayActionToMapAction(item.resolvedAction())
        }
        val mapFamily = OverlaySystemFamily.toMapFamily(family)
        val current = buttonMapDocument.profile(mapFamily)
        val next = current.copy(
            select = storedAction(OverlayControlKind.Select),
            start = storedAction(OverlayControlKind.Start),
            l = storedAction(OverlayControlKind.ShoulderL),
            r = storedAction(OverlayControlKind.ShoulderR),
            l2 = storedAction(OverlayControlKind.ShoulderL2),
            r2 = storedAction(OverlayControlKind.ShoulderR2),
            l3 = storedAction(OverlayControlKind.LeftStick),
            r3 = storedAction(OverlayControlKind.RightStick),
        )
        buttonMapDocument = buttonMapDocument.withProfile(mapFamily, next)
        persistButtonMapDocument()
        _state.update {
            if (it.editingOverlayFamily == family) it.copy(editingMapProfile = next) else it
        }
    }

    private fun overlayActionToMapAction(action: OverlayAction): ControllerMapAction = when (action) {
        OverlayAction.Default -> ControllerMapAction.Default
        OverlayAction.ButtonA -> ControllerMapAction.A
        OverlayAction.ButtonB -> ControllerMapAction.B
        OverlayAction.ButtonX -> ControllerMapAction.X
        OverlayAction.ButtonY -> ControllerMapAction.Y
        OverlayAction.ButtonL -> ControllerMapAction.L
        OverlayAction.ButtonR -> ControllerMapAction.R
        OverlayAction.ButtonL2 -> ControllerMapAction.L2
        OverlayAction.ButtonR2 -> ControllerMapAction.R2
        OverlayAction.Select -> ControllerMapAction.Select
        OverlayAction.Start -> ControllerMapAction.Start
        OverlayAction.Menu -> ControllerMapAction.Menu
        OverlayAction.LeftStick -> ControllerMapAction.LeftStick
        OverlayAction.RightStick -> ControllerMapAction.RightStick
        OverlayAction.FastForward -> ControllerMapAction.FastForward
        OverlayAction.ScreenSwap -> ControllerMapAction.ScreenSwap
    }

    private fun updateEditingMapProfile(transform: (ControllerMapProfile) -> ControllerMapProfile) {
        if (profileUsernameOrNull() == null) return
        val overlayFamily = _state.value.editingOverlayFamily
        val mapFamily = OverlaySystemFamily.toMapFamily(overlayFamily)
        val next = transform(mapProfileFor(overlayFamily))
        buttonMapDocument = buttonMapDocument.withProfile(mapFamily, next)
        persistButtonMapDocument()
        // Mirror face swaps into overlay prefs so touch pad matches physical feel.
        val overlay = profileFor(overlayFamily).copy(swapNw = next.swapNw, swapSe = next.swapSe)
        overlayProfiles[overlayFamily] = overlay
        OverlayProfileStore.save(prefs, overlayFamily, overlay)
        persistOverlayProfiles()
        _state.update {
            val live = !it.playing || sessionFamily == overlayFamily
            it.copy(
                editingMapProfile = next,
                editingOverlayProfile = overlay,
                swapNw = if (live) next.swapNw else it.swapNw,
                swapSe = if (live) next.swapSe else it.swapSe,
            )
        }
        applyLiveOverlayFrom(overlayFamily, overlay)
    }

    private fun applyLiveOverlayFrom(family: OverlaySystemFamily, profile: OverlayProfile) {
        if (!_state.value.playing || family != sessionFamily) return
        val orientation = _state.value.overlayOrientation
        val mapProfile = mapProfileFor(family)
        _state.update {
            it.copy(
                padLayout = profile.resolveLayout(sessionSystemKey),
                overlayOpacity = profile.clampedOpacity(),
                swapNw = mapProfile.swapNw,
                swapSe = mapProfile.swapSe,
                overlayItems = profile.resolveItems(sessionSystemKey, orientation),
            )
        }
    }

    private fun publishEditing(family: OverlaySystemFamily) {
        val profile = profileFor(family)
        val mapProfile = mapProfileFor(family)
        val orientation = _state.value.overlayOrientation
        _state.update {
            it.copy(
                editingOverlayFamily = family,
                editingOverlayProfile = profile,
                editingMapProfile = mapProfile,
                overlayItems = if (it.playing) {
                    it.overlayItems
                } else {
                    profile.resolveItems(null, orientation)
                },
            )
        }
    }

    /** Keep play / editor items in sync with phone rotation. */
    fun setOverlayOrientation(isPortrait: Boolean) {
        val orientation = OverlayOrientation.fromPortrait(isPortrait)
        val snap = _state.value
        if (snap.overlayOrientation == orientation) return
        if (snap.overlayEditing) {
            // Persist current draft into the orientation we're leaving, then show the other.
            val leaving = snap.overlayOrientation
            val landscape = if (leaving == OverlayOrientation.Landscape) {
                snap.overlayItems
            } else {
                snap.overlayEditLandscape
            }
            val portrait = if (leaving == OverlayOrientation.Portrait) {
                snap.overlayItems
            } else {
                snap.overlayEditPortrait
            }
            val preset = profileFor(snap.editingOverlayFamily)
                .resolveLayout(if (snap.playing) sessionSystemKey else null)
                .let { OverlayPresets.forLayout(it) }
            val nextLandscape = landscape.ifEmpty { preset }
            val nextPortrait = portrait.ifEmpty { preset }
            val shown = when (orientation) {
                OverlayOrientation.Landscape -> nextLandscape
                OverlayOrientation.Portrait -> nextPortrait
            }
            _state.update {
                it.copy(
                    overlayOrientation = orientation,
                    overlayEditLandscape = nextLandscape,
                    overlayEditPortrait = nextPortrait,
                    overlayItems = shown,
                )
            }
            return
        }
        val family = if (snap.playing) sessionFamily else snap.editingOverlayFamily
        val profile = profileFor(family)
        val systemKey = if (snap.playing) sessionSystemKey else null
        _state.update {
            it.copy(
                overlayOrientation = orientation,
                overlayItems = profile.resolveItems(systemKey, orientation),
            )
        }
    }

    fun setEditingOverlayFamily(family: OverlaySystemFamily) {
        publishEditing(family)
    }

    fun setOverlayLayoutMode(mode: OverlayLayoutMode) {
        updateEditingProfile { profile ->
            when {
                mode == OverlayLayoutMode.Custom && profile.custom == null -> profile
                else -> profile.copy(layoutMode = mode)
            }
        }
    }

    fun setOverlaySwapNw(value: Boolean) {
        updateEditingMapProfile { it.copy(swapNw = value) }
    }

    fun setOverlaySwapSe(value: Boolean) {
        updateEditingMapProfile { it.copy(swapSe = value) }
    }

    fun setOverlayOpacity(value: Float) {
        updateEditingProfile {
            it.copy(opacity = value.coerceIn(OverlayProfile.MIN_OPACITY, OverlayProfile.MAX_OPACITY))
        }
    }

    fun clearOverlayCustom() {
        updateEditingProfile { profile ->
            profile.copy(
                custom = null,
                layoutMode = if (profile.layoutMode == OverlayLayoutMode.Custom) {
                    OverlayLayoutMode.Auto
                } else {
                    profile.layoutMode
                },
            )
        }
    }

    fun resetOverlayProfile() {
        if (profileUsernameOrNull() == null) return
        val family = _state.value.editingOverlayFamily
        OverlayProfileStore.reset(prefs, family)
        overlayProfiles[family] = OverlayProfile.DEFAULT
        persistOverlayProfiles()
        publishEditing(family)
        applyLiveOverlayFrom(family, OverlayProfile.DEFAULT)
        if (!_state.value.playing) {
            val orientation = _state.value.overlayOrientation
            _state.update {
                it.copy(overlayItems = OverlayProfile.DEFAULT.resolveItems(null, orientation))
            }
        }
    }

    /** Enter layout editor (play drawer or Controls). Edits the active family. */
    fun beginOverlayEdit() {
        if (profileUsernameOrNull() == null) return
        val snap = _state.value
        val family = if (snap.playing) sessionFamily else snap.editingOverlayFamily
        val profile = profileFor(family)
        val systemKey = if (snap.playing) sessionSystemKey else null
        val preset = OverlayPresets.forLayout(profile.resolveLayout(systemKey))
        val landscape = profile.custom?.landscape?.ifEmpty { null } ?: preset
        val portrait = profile.custom?.portrait?.ifEmpty { null } ?: preset
        val orientation = snap.overlayOrientation
        val shown = when (orientation) {
            OverlayOrientation.Landscape -> landscape
            OverlayOrientation.Portrait -> portrait
        }
        _state.update {
            it.copy(
                section = if (it.playing) NavSection.Games else it.section,
                overlayEditing = true,
                overlayEditNaming = false,
                editingOverlayFamily = family,
                editingOverlayProfile = profile,
                overlayEditLandscape = landscape,
                overlayEditPortrait = portrait,
                overlayItems = shown,
                overlayEditNameDraft = profile.custom?.name ?: OverlayCustomLayout.DEFAULT_NAME,
            )
        }
        // Relax pause only for control editing (live game under the editor).
        syncMenuPause()
        refreshPhysicalPads()
    }

    fun updateOverlayItems(items: List<OverlayItem>) {
        if (!_state.value.overlayEditing) return
        val clamped = items.map { it.clamped() }
        _state.update { snap ->
            when (snap.overlayOrientation) {
                OverlayOrientation.Landscape -> snap.copy(
                    overlayItems = clamped,
                    overlayEditLandscape = clamped,
                )
                OverlayOrientation.Portrait -> snap.copy(
                    overlayItems = clamped,
                    overlayEditPortrait = clamped,
                )
            }
        }
    }

    /** Done in the visual editor → name (or rename) the single custom slot. */
    fun finishOverlayEdit() {
        val snap = _state.value
        if (!snap.overlayEditing) return
        // Flush current orientation into the draft pair.
        val landscape = if (snap.overlayOrientation == OverlayOrientation.Landscape) {
            snap.overlayItems
        } else {
            snap.overlayEditLandscape
        }
        val portrait = if (snap.overlayOrientation == OverlayOrientation.Portrait) {
            snap.overlayItems
        } else {
            snap.overlayEditPortrait
        }
        val existingName = profileFor(snap.editingOverlayFamily).custom?.name
            ?: OverlayCustomLayout.DEFAULT_NAME
        _state.update {
            it.copy(
                overlayEditNaming = true,
                overlayEditNameDraft = existingName,
                overlayEditLandscape = landscape,
                overlayEditPortrait = portrait,
            )
        }
    }

    fun setOverlayEditNameDraft(value: String) {
        _state.update {
            it.copy(overlayEditNameDraft = value.take(OverlayCustomLayout.MAX_NAME_LEN))
        }
    }

    fun cancelOverlayEditNaming() {
        _state.update { it.copy(overlayEditNaming = false) }
    }

    fun confirmOverlayEditName() {
        val name = _state.value.overlayEditNameDraft
        saveOverlayCustom(name)
    }

    private fun saveOverlayCustom(rawName: String) {
        if (profileUsernameOrNull() == null) return
        val snap = _state.value
        if (!snap.overlayEditing) return
        val family = snap.editingOverlayFamily
        val custom = OverlayCustomLayout(
            name = rawName,
            landscape = snap.overlayEditLandscape,
            portrait = snap.overlayEditPortrait,
        ).let { it.copy(name = it.clampedName()) }
        val next = profileFor(family).copy(
            layoutMode = OverlayLayoutMode.Custom,
            custom = custom,
        )
        overlayProfiles[family] = next
        OverlayProfileStore.save(prefs, family, next)
        persistOverlayProfiles()
        syncButtonMapRemapsFromItems(family, custom.itemsFor(snap.overlayOrientation))
        _state.update {
            it.copy(
                overlayEditing = false,
                overlayEditNaming = false,
                editingOverlayProfile = next,
                overlayItems = next.resolveItems(
                    if (it.playing) sessionSystemKey else null,
                    it.overlayOrientation,
                ),
                overlayEditNameDraft = custom.name,
            )
        }
        applyLiveOverlayFrom(family, next)
        syncMenuPause()
        refreshPhysicalPads()
    }

    /** Discard in-progress editor without saving. */
    fun cancelOverlayEdit() {
        val snap = _state.value
        if (!snap.overlayEditing) return
        val family = snap.editingOverlayFamily
        val profile = profileFor(family)
        _state.update {
            it.copy(
                overlayEditing = false,
                overlayEditNaming = false,
                overlayItems = profile.resolveItems(
                    if (it.playing) sessionSystemKey else null,
                    it.overlayOrientation,
                ),
            )
        }
        syncMenuPause()
        refreshPhysicalPads()
    }

    /**
     * Prefer a USB/Bluetooth pad over the touch overlay.
     * Face swaps and other control config still apply to the physical pad.
     * When enabled with no pad attached, the overlay stays as fallback.
     */
    fun setUsePhysicalController(enabled: Boolean) {
        prefs.edit().putBoolean(KEY_USE_PHYSICAL, enabled).apply()
        refreshPhysicalPads(preferPhysical = enabled)
        if (!enabled) {
            gamepadTracker.reset()
            latestPad = ControllerState()
        }
    }

    fun refreshPhysicalPads(preferPhysical: Boolean = _state.value.usePhysicalController) {
        val pads = PhysicalGamepad.connectedPads()
        val connected = pads.isNotEmpty()
        val label = pads.firstOrNull()?.name.orEmpty()
        val editing = _state.value.overlayEditing
        val active = preferPhysical && connected && !editing
        val wasActive = _state.value.physicalInputActive
        _state.update {
            it.copy(
                usePhysicalController = preferPhysical,
                physicalPadConnected = connected,
                physicalPadLabel = label,
                physicalInputActive = active,
            )
        }
        if (wasActive != active) {
            gamepadTracker.reset()
            latestPad = ControllerState()
        }
    }

    fun requestPlayMenu() {
        _playMenuRequests.tryEmit(Unit)
    }

    /** @return true if consumed as physical play input. */
    fun onGamepadKeyEvent(event: KeyEvent): Boolean {
        if (menuDrawerOpen && _state.value.playing) {
            return handlePlayMenuGamepadKey(event)
        }
        if (!_state.value.physicalInputActive) return false
        return gamepadTracker.handleKeyEvent(event)
    }

    /** @return true if consumed as physical play input. */
    fun onGamepadMotionEvent(event: MotionEvent): Boolean {
        if (menuDrawerOpen && _state.value.playing) {
            return handlePlayMenuGamepadMotion(event)
        }
        if (!_state.value.physicalInputActive) return false
        return gamepadTracker.handleMotionEvent(event)
    }

    /**
     * Hardware keyboard while playing.
     *
     * Priority:
     * 1. Soft keyboard dialog up → leave keys alone (Enter/Backspace type in the OSK).
     * 2. Play menu open → keys navigate/select/close the drawer only.
     * 3. Otherwise → Backspace opens menu; arrows = joypad D-pad; P/Space/Enter/… remoted.
     *
     * Events from a real [PhysicalGamepad] device are left to [onGamepadKeyEvent]
     * (MainActivity routes those first).
     */
    fun onPlayKeyEvent(event: KeyEvent): Boolean {
        val snap = _state.value
        if (!snap.playing) return false
        // OSK owns Enter/Backspace/letters while its dialog is showing.
        if (snap.softKeyboard != null) return false
        if (snap.section == NavSection.Controls ||
            snap.section == NavSection.GameOptions ||
            snap.section == NavSection.Stream ||
            snap.section == NavSection.Settings ||
            snap.section == NavSection.Remote
        ) {
            return false
        }
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) {
            return false
        }
        // Pad devices: only Backspace opens the menu from this path; face/D-pad stay on tracker.
        if (PhysicalGamepad.isGameControllerDeviceId(event.deviceId)) {
            if (menuDrawerOpen) return false
            if (event.keyCode == KeyEvent.KEYCODE_DEL) {
                if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
                    requestPlayMenu()
                }
                return true
            }
            return false
        }
        val down = event.action == KeyEvent.ACTION_DOWN
        val edge = down && event.repeatCount == 0
        val keyCode = event.keyCode

        if (menuDrawerOpen) {
            return handlePlayMenuKey(keyCode, edge)
        }

        // Backspace → play menu (never remoted on mobile while playing).
        if (keyCode == KeyEvent.KEYCODE_DEL) {
            if (edge) requestPlayMenu()
            return true
        }

        // Arrows → joypad D-pad for the game.
        keyboardDpadMask(keyCode)?.let { mask ->
            setKeyboardDpad(mask, down)
            return true
        }

        // F → hold EmulatorControl fast-forward (down=on, up=off). Not a toggle —
        // menu latch is the only sticky On.
        if (keyCode == KeyEvent.KEYCODE_F) {
            setFastForwardHold(down)
            return true
        }

        // P → absolute pause toggle (same ownership as menu pause). Never remoted:
        // host remoted-P is a cache toggle that desyncs EmulatorControl On/Off.
        if (keyCode == KeyEvent.KEYCODE_P) {
            if (edge) {
                setPaused(!_state.value.paused, force = true)
            }
            return true
        }

        val bit = remotedKeyBitFromAndroidKeyCode(keyCode) ?: return false
        setRemotedKey(bit, down)
        return true
    }

    fun isPhysicalGamepadDevice(deviceId: Int): Boolean =
        PhysicalGamepad.isGameControllerDeviceId(deviceId)

    private fun handlePlayMenuKey(keyCode: Int, edge: Boolean): Boolean {
        if (isPlayMenuCloseKey(keyCode)) {
            if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.Close)
            return true
        }
        if (isPlayMenuActivateKey(keyCode)) {
            if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.Activate)
            return true
        }
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.MoveUp)
                return true
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.MoveDown)
                return true
            }
            else -> Unit
        }
        // Consume other remoted keys so they do not hit the host while the menu owns input.
        if (keyCode == KeyEvent.KEYCODE_F ||
            keyCode == KeyEvent.KEYCODE_P ||
            remotedKeyBitFromAndroidKeyCode(keyCode) != null
        ) {
            return true
        }
        return false
    }

    private fun handlePlayMenuGamepadKey(event: KeyEvent): Boolean {
        if (!PhysicalGamepad.isGameControllerDeviceId(event.deviceId)) return false
        if (event.action != KeyEvent.ACTION_DOWN && event.action != KeyEvent.ACTION_UP) {
            return true
        }
        val edge = event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0
        val keyCode = event.keyCode
        // Keyboard-like keys on hybrid devices: let [onPlayKeyEvent] own them (already ran).
        if (keyCode == KeyEvent.KEYCODE_DEL ||
            keyCode == KeyEvent.KEYCODE_ENTER ||
            keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER ||
            keyCode == KeyEvent.KEYCODE_ESCAPE ||
            keyCode == KeyEvent.KEYCODE_SPACE ||
            keyCode == KeyEvent.KEYCODE_P ||
            keyCode == KeyEvent.KEYCODE_TAB ||
            keyCode == KeyEvent.KEYCODE_F ||
            keyCode == KeyEvent.KEYCODE_F1 ||
            keyCode == KeyEvent.KEYCODE_F8
        ) {
            return false
        }
        when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_HOME -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.Close)
            }
            KeyEvent.KEYCODE_DPAD_UP -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.MoveUp)
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.MoveDown)
            }
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.Activate)
            }
            KeyEvent.KEYCODE_BUTTON_B -> {
                if (edge) _playMenuCommands.tryEmit(PlayMenuCommand.Close)
            }
            else -> Unit
        }
        // Swallow remaining pad buttons while the menu owns controls.
        return true
    }

    private fun handlePlayMenuGamepadMotion(event: MotionEvent): Boolean {
        if (!PhysicalGamepad.isGameControllerDeviceId(event.deviceId)) return false
        val sources = event.source
        val fromStick =
            sources and android.view.InputDevice.SOURCE_JOYSTICK ==
                android.view.InputDevice.SOURCE_JOYSTICK ||
                sources and android.view.InputDevice.SOURCE_GAMEPAD ==
                android.view.InputDevice.SOURCE_GAMEPAD
        if (!fromStick) return false
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val up = hatY < -0.5f
        val down = hatY > 0.5f
        if (up && !menuHatUp) _playMenuCommands.tryEmit(PlayMenuCommand.MoveUp)
        if (down && !menuHatDown) _playMenuCommands.tryEmit(PlayMenuCommand.MoveDown)
        menuHatUp = up
        menuHatDown = down
        return true
    }

    private fun setRemotedKey(bit: Int, down: Boolean) {
        val next = remotedKeysHeld.updateAndGet { cur ->
            if (down) cur or bit else cur and bit.inv()
        }
        logControl(
            "keyboard remoted bit=0x${bit.toString(16)} down=$down held=0x${next.toString(16)}",
        )
    }

    private fun setKeyboardDpad(mask: Int, down: Boolean) {
        val next = keyboardDpadBits.updateAndGet { cur ->
            if (down) cur or mask else cur and mask.inv()
        }
        logControl(
            "keyboard dpad mask=0x${mask.toString(16)} down=$down held=0x${next.toString(16)}",
        )
    }

    private fun clearRemotedKeys() {
        remotedKeysHeld.set(0)
    }

    private fun clearKeyboardDpad() {
        keyboardDpadBits.set(0)
    }

    private fun padForSend(): ControllerState {
        if (menuDrawerOpen) return ControllerState()
        val kb = keyboardDpadBits.get()
        if (kb == 0) return latestPad
        return latestPad.copy(buttons = latestPad.buttons or kb)
    }

    private fun onPhysicalPadState(state: ControllerState) {
        if (!_state.value.physicalInputActive) {
            logControl("physical pad ignored (physicalInputActive=false) ${formatPad(state)}")
            return
        }
        if (menuDrawerOpen) {
            logControl("physical pad ignored (menuDrawerOpen) ${formatPad(state)}")
            return
        }
        // Remaps come from the overlay Action editor; face swaps from the shared map profile.
        val map = mapProfileFor(sessionFamily)
        val swapped = ControllerState.applyFaceButtonSwaps(state, map.swapNw, map.swapSe)
        latestPad = swapped
        logControlPad(
            "physical",
            swapped,
            "swapNw=${map.swapNw} swapSe=${map.swapSe}",
        )
    }

    /** Overlay Action remap for a control kind (Custom layout); else the kind default. */
    private fun overlayActionFor(kind: OverlayControlKind): OverlayAction {
        val item = _state.value.overlayItems.firstOrNull { it.kind == kind }
        return item?.resolvedAction() ?: kind.defaultAction
    }

    private fun resolveInitialPreferPhysical(): Boolean {
        if (prefs.contains(KEY_USE_PHYSICAL)) {
            return prefs.getBoolean(KEY_USE_PHYSICAL, false)
        }
        // First launch: virtual overlay unless a controller is already attached.
        val found = PhysicalGamepad.connectedPads().isNotEmpty()
        prefs.edit().putBoolean(KEY_USE_PHYSICAL, found).apply()
        return found
    }

    private fun updateEditingProfile(transform: (OverlayProfile) -> OverlayProfile) {
        if (profileUsernameOrNull() == null) return
        val family = _state.value.editingOverlayFamily
        val next = transform(profileFor(family))
        overlayProfiles[family] = next
        OverlayProfileStore.save(prefs, family, next)
        persistOverlayProfiles()
        publishEditing(family)
        applyLiveOverlayFrom(family, next)
    }

    private fun qualityFromPrefs(): MediaQualityTier {
        val id = prefs.getInt(KEY_STREAM_QUALITY, MediaQualityTier.Medium.id)
        return MediaQualityTier.entries.firstOrNull { it.id == id } ?: MediaQualityTier.Medium
    }

    private fun sizeFromPrefs(): MediaStreamSize {
        val id = prefs.getInt(KEY_STREAM_SIZE, MediaStreamSize.P540.id)
        return MediaStreamSize.entries.firstOrNull { it.id == id } ?: MediaStreamSize.P540
    }

    private fun feelFromPrefs(): MediaStreamFeel {
        val id = prefs.getInt(KEY_STREAM_FEEL, MediaStreamFeel.LowLatency.id)
        return MediaStreamFeel.entries.firstOrNull { it.id == id } ?: MediaStreamFeel.LowLatency
    }

    private fun applyStreamPrefsToSession() {
        val snap = _state.value
        session?.wantedTier = snap.streamQuality.id
        session?.wantedSize = snap.streamSize.id
        session?.wantedFeel = snap.streamFeel.id
        // Android always requests Hybrid; portrait stacking is client-side.
        session?.displayLayout = DisplayLayoutPreference.Landscape.id
    }

    fun selectSection(section: NavSection) {
        if (_state.value.playing) {
            // Stay in the session — open overlay/stream/settings/remote over the play surface.
            val remoteOk =
                section == NavSection.Remote && _state.value.canAccessRemoteDuringPlay()
            if (section == NavSection.Controls ||
                section == NavSection.GameOptions ||
                section == NavSection.Stream ||
                section == NavSection.Settings ||
                remoteOk
            ) {
                if (section == NavSection.Controls) {
                    publishEditing(sessionFamily)
                }
                _state.update { it.copy(section = section) }
                return
            }
            endSession()
            _state.update {
                it.copy(
                    playing = false,
                    videoPlayer = null,
                    selectedGame = null,
                    mediaHint = "",
                    softKeyboard = null,
                )
            }
        }
        if (section == NavSection.Games && !_state.value.connected) {
            _state.update {
                it.copy(
                    section = NavSection.Client,
                    status = "Connect on the Client tab before browsing games.",
                )
            }
            return
        }
        _state.update { it.copy(section = section) }
    }

    /** Dismiss overlay/stream settings and return to the live play surface. */
    fun returnToPlay() {
        if (!_state.value.playing) return
        _state.update { it.copy(section = NavSection.Games) }
    }

    fun onHostChange(value: String) {
        val host = value.trim()
        hostEditSuppressUntilMs = System.currentTimeMillis() + HOST_EDIT_SUPPRESS_MS
        prefs.edit().putString(KEY_HOST, host).apply()
        _state.update {
            it.copy(
                host = host,
                recentGameIds = loadRecentGameIds(host, it.controlPort),
            )
        }
    }

    fun onAltHostChange(value: String) {
        val trimmed = value.trim()
        // Persist blank or a valid IP; keep invalid drafts in UI until corrected.
        if (trimmed.isEmpty() || HostAddresses.looksLikeIp(trimmed)) {
            prefs.edit().putString(KEY_ALT_HOST, trimmed).apply()
        }
        _state.update { it.copy(altHost = value) }
    }

    fun selectDiscoveredHost(host: DiscoveredHost) {
        hostEditSuppressUntilMs = System.currentTimeMillis() + HOST_EDIT_SUPPRESS_MS
        applyDiscoveredHost(host, userSelected = true)
    }

    private fun applyDiscoveredHost(host: DiscoveredHost, userSelected: Boolean) {
        val control = host.controlPort.toString()
        val input = host.inputPort.toString()
        rememberDiscoveredSeed(host.address)
        prefs.edit()
            .putString(KEY_HOST, host.address)
            .putString(KEY_CONTROL_PORT, control)
            .putString(KEY_INPUT_PORT, input)
            .apply()
        val label = if (userSelected) {
            "Selected ${host.username} @ ${host.address}"
        } else {
            "Found ${host.username} @ ${host.address}"
        }
        _state.update {
            it.copy(
                host = host.address,
                controlPort = control,
                inputPort = input,
                recentGameIds = loadRecentGameIds(host.address, control),
                discoveryStatus = label,
                status = if (it.connected || it.playing) it.status else label,
            )
        }
    }

    private fun rememberDiscoveredSeed(address: String) {
        if (address.isBlank() || HostDiscovery.isLoopback(address)) return
        val current = loadRecentDiscoverySeeds().toMutableList()
        current.remove(address)
        current.add(0, address)
        prefs.edit()
            .putString(KEY_DISCOVERY_SEEDS, current.take(MAX_DISCOVERY_SEEDS).joinToString(","))
            .apply()
    }

    private fun loadRecentDiscoverySeeds(): List<String> =
        prefs.getString(KEY_DISCOVERY_SEEDS, "")
            .orEmpty()
            .split(',')
            .map { it.trim() }
            .filter { it.isNotEmpty() }

    private fun startHostDiscovery() {
        if (discoveryJob?.isActive == true) return
        discoveryAttempts = 0
        discoveryJob = viewModelScope.launch(Dispatchers.IO) {
            val browser = runCatching { HostDiscovery() }.getOrElse { error ->
                withContext(Dispatchers.Main) {
                    _state.update {
                        it.copy(discoveryStatus = "Host search unavailable: ${error.message}")
                    }
                }
                return@launch
            }
            try {
                withContext(Dispatchers.Main) {
                    _state.update { it.copy(discoveryStatus = "Looking for a host…") }
                }
                while (isActive) {
                    val playing = _state.value.playing
                    if (!playing) {
                        val saved = _state.value.host.trim()
                        val alt = _state.value.altHost.trim()
                        val seeds = linkedSetOf<String>()
                        if (saved.isNotEmpty() && !HostDiscovery.isLoopback(saved)) {
                            seeds.add(saved)
                        }
                        if (alt.isNotEmpty() &&
                            HostAddresses.looksLikeIp(alt) &&
                            !HostDiscovery.isLoopback(alt)
                        ) {
                            seeds.add(alt)
                        }
                        seeds.addAll(loadRecentDiscoverySeeds())
                        browser.setSeedHosts(seeds.toList())
                        runCatching {
                            browser.poll()
                            browser.expireOlderThan(8_000L)
                        }
                        val live = browser.hosts()
                        withContext(Dispatchers.Main) {
                            applyDiscoveryTick(live)
                        }
                    }
                    delay(1_000L)
                }
            } finally {
                browser.close()
            }
        }
    }

    private fun applyDiscoveryTick(live: List<DiscoveredHost>) {
        _state.update { it.copy(discoveredHosts = live) }
        if (_state.value.playing || _state.value.busy) return
        if (System.currentTimeMillis() < hostEditSuppressUntilMs) return

        val saved = _state.value.host.trim()
        val savedLive = live.firstOrNull { it.address == saved }
        if (savedLive != null) {
            // Keep saved address; refresh ports from announce if they drifted.
            if (savedLive.controlPort.toString() != _state.value.controlPort ||
                savedLive.inputPort.toString() != _state.value.inputPort
            ) {
                prefs.edit()
                    .putString(KEY_CONTROL_PORT, savedLive.controlPort.toString())
                    .putString(KEY_INPUT_PORT, savedLive.inputPort.toString())
                    .apply()
                _state.update {
                    it.copy(
                        controlPort = savedLive.controlPort.toString(),
                        inputPort = savedLive.inputPort.toString(),
                        discoveryStatus = "Using ${savedLive.username} @ ${savedLive.address}",
                    )
                }
            } else {
                _state.update {
                    it.copy(discoveryStatus = "Using ${savedLive.username} @ ${savedLive.address}")
                }
            }
            discoveryAttempts = 0
            return
        }

        // Saved missing or empty — pick a live host (same-/24 preferred).
        val preferred = HostDiscovery.preferDiscoveredHost(live)
        if (preferred != null && preferred.address != saved) {
            applyDiscoveredHost(preferred, userSelected = false)
            discoveryAttempts = 0
            return
        }

        discoveryAttempts++
        if (live.isEmpty()) {
            val looking = if (discoveryAttempts <= 8) {
                "Looking for a host…"
            } else {
                "No host found yet — check Wi‑Fi/VPN or enter an IP."
            }
            _state.update { it.copy(discoveryStatus = looking) }
        }
    }

    fun onControlPortChange(value: String) {
        val port = value.trim()
        prefs.edit().putString(KEY_CONTROL_PORT, port).apply()
        _state.update {
            it.copy(
                controlPort = port,
                recentGameIds = loadRecentGameIds(it.host, port),
            )
        }
    }

    fun onInputPortChange(value: String) {
        val port = value.trim()
        _state.update { it.copy(inputPort = port) }
        prefs.edit().putString(KEY_INPUT_PORT, port).apply()
    }

    fun onRemoteSshHostChange(value: String) {
        _state.update { it.copy(remoteSshHost = value) }
        prefs.edit().putString(KEY_REMOTE_SSH_HOST, value.trim()).apply()
    }

    fun onRemoteSshUserChange(value: String) {
        _state.update { it.copy(remoteSshUser = value) }
        prefs.edit().putString(KEY_REMOTE_SSH_USER, value.trim()).apply()
    }

    fun onRemoteSshPasswordChange(value: String) {
        _state.update { it.copy(remoteSshPassword = value) }
    }

    fun onRemoteSshPortChange(value: String) {
        _state.update { it.copy(remoteSshPort = value) }
        prefs.edit().putString(KEY_REMOTE_SSH_PORT, value.trim()).apply()
    }

    fun onRemoteDirectoryChange(value: String) {
        _state.update { it.copy(remoteDirectory = value) }
        prefs.edit().putString(KEY_REMOTE_DIRECTORY, value.trim()).apply()
    }

    fun onRemoteRomRootChange(value: String) {
        _state.update { it.copy(remoteRomRoot = value) }
        prefs.edit().putString(KEY_REMOTE_ROM_ROOT, value.trim()).apply()
    }

    fun onRemoteBinaryChange(value: String) {
        _state.update { it.copy(remoteBinary = value) }
        prefs.edit().putString(KEY_REMOTE_BINARY, value.trim()).apply()
    }

    fun onRemoteStartScriptChange(value: String) {
        _state.update { it.copy(remoteStartScript = value) }
        prefs.edit().putString(KEY_REMOTE_START_SCRIPT, value.trim()).apply()
    }

    fun onRemoteGpuChange(value: String) {
        _state.update { it.copy(remoteGpu = value) }
        prefs.edit().putString(KEY_REMOTE_GPU, value.trim()).apply()
    }

    fun onRemoteBaseControlPortChange(value: String) {
        _state.update { it.copy(remoteBaseControlPort = value) }
        prefs.edit().putString(KEY_REMOTE_BASE_CONTROL, value.trim()).apply()
    }

    fun onRemoteBaseInputPortChange(value: String) {
        _state.update { it.copy(remoteBaseInputPort = value) }
        prefs.edit().putString(KEY_REMOTE_BASE_INPUT, value.trim()).apply()
    }

    private fun setRemoteStatus(status: String, busy: Boolean? = null) {
        ClientFileLog.append("[remote] $status")
        _state.update {
            if (busy == null) it.copy(remoteStatus = status)
            else it.copy(remoteBusy = busy, remoteStatus = status)
        }
    }

    fun ensureRemoteHost() {
        val snap = _state.value
        if (snap.remoteBusy) return
        val host = snap.remoteSshHost.trim()
        val user = snap.remoteSshUser.trim()
        val password = snap.remoteSshPassword
        val directory = snap.remoteDirectory.trim()
        val romRoot = snap.remoteRomRoot.trim()
        val binary = snap.remoteBinary.trim().ifBlank { "./host_runner" }
        val startScript = snap.remoteStartScript.trim()
        val wantGpu = snap.remoteGpu.trim()
        val sshPort = snap.remoteSshPort.trim().toIntOrNull() ?: RemoteHost.DEFAULT_SSH_PORT
        val baseControl = snap.remoteBaseControlPort.trim().toIntOrNull()
            ?: Protocol.DEFAULT_CONTROL_PORT
        val baseInput = snap.remoteBaseInputPort.trim().toIntOrNull()
            ?: Protocol.DEFAULT_INPUT_PORT

        // Fields already persist on each change; flush again so a failed Ensure still keeps them.
        prefs.edit()
            .putString(KEY_REMOTE_SSH_HOST, host)
            .putString(KEY_REMOTE_SSH_USER, user)
            .putString(KEY_REMOTE_SSH_PORT, snap.remoteSshPort.trim().ifBlank { "22" })
            .putString(KEY_REMOTE_DIRECTORY, directory)
            .putString(KEY_REMOTE_ROM_ROOT, romRoot)
            .putString(KEY_REMOTE_BINARY, binary)
            .putString(KEY_REMOTE_START_SCRIPT, startScript)
            .putString(KEY_REMOTE_GPU, wantGpu)
            .putString(KEY_REMOTE_BASE_CONTROL, baseControl.toString())
            .putString(KEY_REMOTE_BASE_INPUT, baseInput.toString())
            .apply()

        if (host.isEmpty() || user.isEmpty() || directory.isEmpty()) {
            setRemoteStatus("SSH host, user, and remote directory are required.")
            return
        }
        if (startScript.isEmpty() && romRoot.isEmpty()) {
            setRemoteStatus(
                "Remote ROM root is required unless a start script is set " +
                    "(Path B: the script owns ROM root / host_runner).",
            )
            return
        }
        if (password.isEmpty()) {
            setRemoteStatus("Enter the SSH password (it is not saved).")
            return
        }

        if (wantGpu.isEmpty()) {
            setRemoteStatus("Probing $host:$baseControl…", busy = true)
        } else {
            setRemoteStatus("Probing $host for a free lobby on GPU “$wantGpu”…", busy = true)
        }

        viewModelScope.launch(Dispatchers.IO) {
            fun applyPorts(control: Int, input: Int, status: String) {
                prefs.edit()
                    .putString(KEY_HOST, host)
                    .putString(KEY_CONTROL_PORT, control.toString())
                    .putString(KEY_INPUT_PORT, input.toString())
                    .putInt(KEY_REMOTE_TRACKED_CONTROL, control)
                    .apply()
                ClientFileLog.append("[remote] $status")
                _state.update {
                    it.copy(
                        remoteBusy = false,
                        remoteStatus = status,
                        host = host,
                        controlPort = control.toString(),
                        inputPort = input.toString(),
                        remoteTrackedControlPort = control,
                        recentGameIds = loadRecentGameIds(host, control.toString()),
                    )
                }
                refreshRemoteUsers()
            }

            fun fail(status: String) {
                setRemoteStatus(status, busy = false)
            }

            val resolvedBinary = RemoteHost.resolveBinary(directory, binary)
            ClientFileLog.append(
                "[remote] ensure host=$host user=$user dir=$directory rom=$romRoot " +
                    "binary=$resolvedBinary ports=$baseControl/$baseInput gpu=${wantGpu.ifEmpty { "(default)" }}",
            )

            var gpuOptions = emptyList<RemoteHost.GpuOption>()
            var resolvedGpuId = ""
            var resolvedGpuLabel = ""

            if (wantGpu.isNotEmpty()) {
                setRemoteStatus("Listing remote GPUs (host_runner --list-gpus)…")
                val listCmd = RemoteHost.listGpusShell(directory, binary)
                ClientFileLog.append("[remote] ssh list-gpus cmd: $listCmd")
                val listed = RemoteHost.runSshCommand(host, sshPort, user, password, listCmd)
                if (!listed.ok) {
                    fail("Remote GPU list failed: ${listed.error}")
                    return@launch
                }
                gpuOptions = RemoteHost.parseListGpusOutput(listed.output)
                val matched = RemoteHost.matchGpuOption(gpuOptions, wantGpu)
                if (matched == null) {
                    val available = gpuOptions.joinToString(", ") { "${it.name} [${it.id}]" }
                        .ifEmpty { "(none)" }
                    fail("No remote GPU matched “$wantGpu”. Available: $available")
                    return@launch
                }
                resolvedGpuId = matched.id
                resolvedGpuLabel = "${matched.name} [${matched.id}]"
                setRemoteStatus("Matched remote GPU $resolvedGpuLabel — probing lobbies…")
            }

            suspend fun queryProcessGpu(controlPort: Int): String {
                if (resolvedGpuId.isEmpty()) return ""
                val cmd = RemoteHost.encodeGpuQueryShell(controlPort)
                val ssh = RemoteHost.runSshCommand(host, sshPort, user, password, cmd, timeoutSec = 15)
                if (!ssh.ok) return ""
                return ssh.output.trim()
            }

            suspend fun startInstance(instanceIndex: Int, ports: RemoteHostPortBlock): Boolean {
                val cmd = RemoteHost.startShell(
                    directory,
                    binary,
                    romRoot,
                    ports,
                    encodeGpu = resolvedGpuId,
                    startScript = startScript,
                )
                var msg = if (startScript.isEmpty()) {
                    "SSH-starting host instance $instanceIndex on port ${ports.controlPort}"
                } else {
                    "SSH-starting via script (instance $instanceIndex, port ${ports.controlPort})"
                }
                if (resolvedGpuLabel.isNotEmpty()) {
                    msg += " ($resolvedGpuLabel)"
                }
                msg += "…"
                setRemoteStatus(msg)
                ClientFileLog.append("[remote] ssh start cmd: $cmd")
                val ssh = RemoteHost.runSshCommand(host, sshPort, user, password, cmd)
                if (!ssh.ok) {
                    fail("SSH start failed: ${ssh.error}")
                    return false
                }
                if (ssh.output.isNotBlank()) {
                    ClientFileLog.append("[remote] ssh stdout: ${ssh.output}")
                }
                repeat(20) {
                    delay(500)
                    if (RemoteHost.probeActiveSession(host, ports.controlPort) != null) {
                        val gpuNote = if (resolvedGpuLabel.isNotEmpty()) " $resolvedGpuLabel" else ""
                        applyPorts(
                            ports.controlPort,
                            ports.inputPort,
                            "Started new host instance on $host:${ports.controlPort}$gpuNote",
                        )
                        return true
                    }
                }
                fail("SSH start reported success but control port ${ports.controlPort} never answered")
                return false
            }

            for (n in 0..8) {
                val ports = RemoteHost.portBlock(n, baseControl, baseInput)
                val info = RemoteHost.probeActiveSession(host, ports.controlPort)
                if (info != null) {
                    val processGpu = queryProcessGpu(ports.controlPort)
                    if (RemoteHost.lobbyUsableForGpu(info, resolvedGpuId, processGpu, gpuOptions)) {
                        val slotText = if (info.activeSlots != null && info.maxSlots != null) {
                            " (slots ${info.activeSlots}/${info.maxSlots})"
                        } else {
                            ""
                        }
                        val gpuText = if (resolvedGpuLabel.isNotEmpty()) " $resolvedGpuLabel" else ""
                        applyPorts(
                            ports.controlPort,
                            ports.inputPort,
                            "Reusing existing host instance on $host:${ports.controlPort}$slotText$gpuText",
                        )
                        return@launch
                    }
                    continue
                }
                if (n == 0) {
                    setRemoteStatus("No host on base port — will SSH-start…")
                }
                startInstance(n, ports)
                return@launch
            }

            fail(
                if (wantGpu.isEmpty()) {
                    "All probed host instances are full."
                } else {
                    val label = resolvedGpuLabel.ifEmpty { wantGpu }
                    "No free lobby on GPU “$label” (existing instances full or different GPU)."
                },
            )
        }
    }

    fun stopRemoteHost() {
        val snap = _state.value
        if (snap.remoteBusy) return
        val host = snap.remoteSshHost.trim()
        val user = snap.remoteSshUser.trim()
        val password = snap.remoteSshPassword
        val directory = snap.remoteDirectory.trim()
        val binary = snap.remoteBinary.trim().ifBlank { "./host_runner" }
        val wantGpu = snap.remoteGpu.trim()
        val sshPort = snap.remoteSshPort.trim().toIntOrNull() ?: RemoteHost.DEFAULT_SSH_PORT
        val baseControl = snap.remoteBaseControlPort.trim().toIntOrNull()
            ?: Protocol.DEFAULT_CONTROL_PORT
        val baseInput = snap.remoteBaseInputPort.trim().toIntOrNull()
            ?: Protocol.DEFAULT_INPUT_PORT
        var control = if (snap.remoteTrackedControlPort > 0) {
            snap.remoteTrackedControlPort
        } else {
            baseControl
        }

        if (host.isEmpty() || user.isEmpty()) {
            setRemoteStatus("SSH host and user are required to stop.")
            return
        }
        if (password.isEmpty()) {
            setRemoteStatus("Enter the SSH password (it is not saved).")
            return
        }

        if (wantGpu.isEmpty()) {
            setRemoteStatus("Stopping remote host on control port $control…", busy = true)
        } else {
            setRemoteStatus("Finding remote host for GPU “$wantGpu” to stop…", busy = true)
        }

        viewModelScope.launch(Dispatchers.IO) {
            var resolvedLabel = ""
            if (wantGpu.isNotEmpty()) {
                val listCmd = RemoteHost.listGpusShell(directory, binary)
                val listed = RemoteHost.runSshCommand(host, sshPort, user, password, listCmd)
                if (!listed.ok) {
                    setRemoteStatus("Remote GPU list failed: ${listed.error}", busy = false)
                    return@launch
                }
                val gpuOptions = RemoteHost.parseListGpusOutput(listed.output)
                val matched = RemoteHost.matchGpuOption(gpuOptions, wantGpu)
                if (matched == null) {
                    setRemoteStatus("No remote GPU matched “$wantGpu”.", busy = false)
                    return@launch
                }
                resolvedLabel = "${matched.name} [${matched.id}]"
                var foundPort: Int? = null
                for (n in 0 until 8) {
                    val ports = RemoteHost.portBlock(n, baseControl, baseInput)
                    val gpuSsh = RemoteHost.runSshCommand(
                        host,
                        sshPort,
                        user,
                        password,
                        RemoteHost.encodeGpuQueryShell(ports.controlPort),
                        timeoutSec = 15,
                    )
                    if (!gpuSsh.ok) continue
                    val processGpu = gpuSsh.output.trim()
                    if (processGpu.isEmpty() && n == 0 && matched.id == gpuOptions.firstOrNull()?.id) {
                        foundPort = ports.controlPort
                        break
                    }
                    val processMatch = RemoteHost.matchGpuOption(gpuOptions, processGpu)
                    if (processMatch?.id == matched.id) {
                        foundPort = ports.controlPort
                        break
                    }
                }
                if (foundPort == null) {
                    setRemoteStatus("No running host_runner found for $resolvedLabel.", busy = false)
                    return@launch
                }
                control = foundPort
                setRemoteStatus("Stopping $resolvedLabel on control port $control…")
            }

            val cmd = RemoteHost.stopShell(control, directory)
            ClientFileLog.append("[remote] ssh stop cmd: $cmd")
            val ssh = RemoteHost.runSshCommand(host, sshPort, user, password, cmd)
            if (ssh.ok) {
                var msg = "Stopped remote host on control port $control."
                if (resolvedLabel.isNotEmpty()) msg += " ($resolvedLabel)"
                _state.update {
                    it.copy(
                        remoteBusy = false,
                        remoteStatus = msg,
                        remoteTrackedControlPort = 0,
                        remoteUsers = emptyList(),
                        remoteSelectedUserIndex = -1,
                    )
                }
            } else {
                setRemoteStatus("SSH stop failed: ${ssh.error}", busy = false)
            }
        }
    }

    fun selectRemoteUser(index: Int) {
        _state.update { it.copy(remoteSelectedUserIndex = index) }
    }

    fun refreshRemoteUsers() {
        val snap = _state.value
        if (snap.remoteBusy) {
            setRemoteStatus("Remote action already running.")
            return
        }
        val host = snap.remoteSshHost.trim()
        val user = snap.remoteSshUser.trim()
        val password = snap.remoteSshPassword
        val sshPort = snap.remoteSshPort.trim().toIntOrNull() ?: RemoteHost.DEFAULT_SSH_PORT
        if (host.isEmpty() || user.isEmpty()) {
            setRemoteStatus("SSH host and user are required.")
            return
        }
        if (password.isEmpty()) {
            setRemoteStatus("Enter the SSH password (it is not saved).")
            return
        }
        setRemoteStatus("Refreshing remote users…", busy = true)
        viewModelScope.launch(Dispatchers.IO) {
            val cmd = RemoteHost.listPresenceShell()
            val ssh = RemoteHost.runSshCommand(host, sshPort, user, password, cmd)
            if (!ssh.ok) {
                setRemoteStatus("Refresh users failed: ${ssh.error}", busy = false)
                return@launch
            }
            val parsed = RemoteHost.parsePresenceOutput(ssh.output)
            val actives = parsed.filter { it.kind == "active" }
            val connected = parsed.filter { it.kind == "connected" }.filter { row ->
                actives.none { active ->
                    active.slotIndex == row.slotIndex &&
                        active.username == row.username &&
                        row.seated
                }
            }
            val rows = actives + connected
            _state.update {
                it.copy(
                    remoteBusy = false,
                    remoteUsers = rows,
                    remoteSelectedUserIndex = -1,
                    remoteStatus = "Remote users: ${rows.size} row(s).",
                )
            }
        }
    }

    fun kickRemoteUser() {
        val snap = _state.value
        if (snap.remoteBusy) {
            setRemoteStatus("Remote action already running.")
            return
        }
        val index = snap.remoteSelectedUserIndex
        val row = snap.remoteUsers.getOrNull(index)
        if (row == null) {
            setRemoteStatus("Select a remote user row first.")
            return
        }
        val host = snap.remoteSshHost.trim()
        val user = snap.remoteSshUser.trim()
        val password = snap.remoteSshPassword
        val sshPort = snap.remoteSshPort.trim().toIntOrNull() ?: RemoteHost.DEFAULT_SSH_PORT
        if (host.isEmpty() || user.isEmpty()) {
            setRemoteStatus("SSH host and user are required.")
            return
        }
        if (password.isEmpty()) {
            setRemoteStatus("Enter the SSH password (it is not saved).")
            return
        }
        val label = row.label()
        setRemoteStatus("Kicking $label…", busy = true)
        viewModelScope.launch(Dispatchers.IO) {
            val cmd = if (row.kind == "active") {
                RemoteHost.kickActiveShell(row.slotIndex)
            } else {
                RemoteHost.kickConnectedShell(row.clientId, row.slotIndex)
            }
            val ssh = RemoteHost.runSshCommand(host, sshPort, user, password, cmd)
            if (!ssh.ok) {
                setRemoteStatus("Kick failed: ${ssh.error}", busy = false)
                return@launch
            }
            setRemoteStatus("Kick requested for $label.", busy = false)
            refreshRemoteUsers()
        }
    }

    fun onUsernameChange(value: String) {
        val username = value.trim().let { raw ->
            if (raw.equals(PLACEHOLDER_USERNAME, ignoreCase = true)) "" else raw
        }
        // Only persist real profile names. Clearing the field must not erase SharedPreferences —
        // Connect/disconnect/restart used to wipe merk by writing "" (or inventing "android").
        if (isProfileUsername(username)) {
            prefs.edit().putString(KEY_USERNAME, username).apply()
        }
        _state.update {
            it.copy(
                username = username,
                hasProfileUsername = isProfileUsername(username) || profileUsernameOrNull() != null,
            )
        }
        reloadControlsFromLocalStore()
        refreshControlsSyncReady()
    }

    /** Re-fill Profile from SharedPreferences if the UI field was cleared without a new name. */
    private fun rehydrateUsernameFromPrefs() {
        if (isProfileUsername(_state.value.username.trim())) return
        val stored = profileUsernameOrNull() ?: return
        _state.update {
            it.copy(username = stored, hasProfileUsername = true)
        }
    }

    /** Saved profile name for host sessions — never the "android" placeholder. */
    private fun sessionUsernameOrNull(): String? {
        rehydrateUsernameFromPrefs()
        val fromState = _state.value.username.trim()
        if (isProfileUsername(fromState)) {
            return fromState
        }
        return profileUsernameOrNull()
    }

    fun pullControlsFromHost() {
        viewModelScope.launch(Dispatchers.IO) {
            _state.update { it.copy(busy = true, status = "Pulling controls…") }
            runCatching {
                val username = requireProfileUsername()
                withControlsSyncConnection { send, receive ->
                    persistOverlayProfiles()
                    persistButtonMapDocument()
                    send(PacketCodec.controlsDbPull(username))
                    when (val packet = receive()) {
                        is IncomingPacket.ControlsDb -> {
                            if (!packet.found || packet.dbBytes.isEmpty()) {
                                error("No controls stored on host for $username")
                            }
                            if (!cadenceControls.importPackBytes(username, packet.dbBytes)) {
                                error("Failed to import controls database")
                            }
                            reloadControlsFromLocalStore()
                            "Pulled controls for $username"
                        }
                        is IncomingPacket.Error -> error(packet.value.message)
                        else -> error("unexpected response: $packet")
                    }
                }
            }.fold(
                onSuccess = { msg ->
                    _state.update { it.copy(busy = false, status = msg) }
                },
                onFailure = { err ->
                    _state.update {
                        it.copy(busy = false, status = "Pull failed: ${err.message}")
                    }
                },
            )
        }
    }

    fun pushControlsToHost() {
        viewModelScope.launch(Dispatchers.IO) {
            _state.update { it.copy(busy = true, status = "Pushing controls…") }
            runCatching {
                val username = requireProfileUsername()
                withControlsSyncConnection { send, receive ->
                    persistOverlayProfiles()
                    persistButtonMapDocument()
                    val bytes = cadenceControls.exportPackBytes(username)
                    send(PacketCodec.controlsDbPush(username, bytes))
                    when (val packet = receive()) {
                        is IncomingPacket.ControlsDbAck -> {
                            if (!packet.ok) error(packet.message.ifBlank { "push rejected" })
                            "Pushed controls for $username"
                        }
                        is IncomingPacket.Error -> error(packet.value.message)
                        else -> error("unexpected response: $packet")
                    }
                }
            }.fold(
                onSuccess = { msg ->
                    _state.update { it.copy(busy = false, status = msg) }
                },
                onFailure = { err ->
                    _state.update {
                        it.copy(busy = false, status = "Push failed: ${err.message}")
                    }
                },
            )
        }
    }

    /**
     * Sync only on an already-authenticated connection (LobbyPresence or live session).
     * Does not dial a temporary socket.
     */
    private suspend fun <T> withControlsSyncConnection(
        block: suspend (
            send: (ByteArray) -> Unit,
            receive: suspend () -> IncomingPacket,
        ) -> T,
    ): T {
        val held = lobbyPresence
        if (held != null && held.isConnected()) {
            return block(
                { bytes -> held.send(bytes) },
                { held.receive() },
            )
        }
        val active = session
        if (active != null && _state.value.playing) {
            val deferred = CompletableDeferred<IncomingPacket>()
            check(controlsSyncWaiter.compareAndSet(null, deferred)) {
                "controls sync already in progress"
            }
            try {
                return block(
                    { bytes -> active.sendControlPacket(bytes) },
                    { deferred.await() },
                )
            } finally {
                controlsSyncWaiter.compareAndSet(deferred, null)
            }
        }
        error("Connect to a host (with password) before syncing controls")
    }

    fun onPasswordChange(value: String) {
        _state.update { it.copy(password = value) }
    }

    fun onNewPasswordChange(value: String) {
        _state.update { it.copy(newPassword = value) }
    }

    fun onConfirmPasswordChange(value: String) {
        _state.update { it.copy(confirmPassword = value) }
    }

    fun onChangeCurrentPasswordChange(value: String) {
        _state.update { it.copy(changeCurrentPassword = value) }
    }

    fun onForcePasswordDraftChange(value: String) {
        _state.update { it.copy(forcePasswordDraft = value) }
    }

    fun onForcePasswordConfirmChange(value: String) {
        _state.update { it.copy(forcePasswordConfirm = value) }
    }

    fun submitForcePasswordChange() {
        val snap = _state.value
        val first = snap.forcePasswordDraft
        val second = snap.forcePasswordConfirm
        if (first.isEmpty() || first != second) {
            _state.update { it.copy(passwordStatus = "New passwords must match and not be empty.") }
            return
        }
        passwordChangeResult = first
        passwordChangeLatch?.countDown()
        _state.update {
            it.copy(
                forcePasswordChange = false,
                forcePasswordDraft = "",
                forcePasswordConfirm = "",
                password = first,
                passwordStatus = "",
            )
        }
    }

    fun cancelForcePasswordChange() {
        passwordChangeResult = ""
        passwordChangeLatch?.countDown()
        _state.update {
            it.copy(
                forcePasswordChange = false,
                forcePasswordDraft = "",
                forcePasswordConfirm = "",
            )
        }
    }

    fun changePasswordOnHost() {
        if (_state.value.busy) return
        val host = _state.value.host.trim()
        if (host.isEmpty()) {
            _state.update { it.copy(passwordStatus = "Set a Host IP on the Client tab first.") }
            return
        }
        val controlPort = _state.value.controlPort.toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT
        val username = sessionUsernameOrNull()
        if (username == null) {
            _state.update {
                it.copy(
                    passwordStatus = "Set a Profile username before changing password.",
                    section = NavSection.Profile,
                )
            }
            return
        }
        val current = _state.value.password.ifEmpty { _state.value.changeCurrentPassword }
        val newPw = _state.value.newPassword
        val confirm = _state.value.confirmPassword
        if (current.isEmpty()) {
            _state.update {
                it.copy(
                    passwordStatus =
                        "Enter your password on the Client tab, or Current password here.",
                )
            }
            return
        }
        if (newPw.isEmpty() || newPw != confirm) {
            _state.update { it.copy(passwordStatus = "New passwords must match and not be empty.") }
            return
        }
        _state.update { it.copy(busy = true, passwordStatus = "Changing password…") }
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    ControlConnection(host, controlPort).use { conn ->
                        conn.connect()
                        conn.send(PacketCodec.passwordChange(username, current, newPw))
                        when (val reply = conn.receive()) {
                            is IncomingPacket.Error -> reply.value.message
                            else -> "unexpected reply from host"
                        }
                    }
                }
            }
            result.fold(
                onSuccess = { message ->
                    _state.update {
                        it.copy(
                            busy = false,
                            passwordStatus = message,
                            password = if (message == "password updated") newPw else it.password,
                            changeCurrentPassword =
                                if (message == "password updated") "" else it.changeCurrentPassword,
                            newPassword = if (message == "password updated") "" else it.newPassword,
                            confirmPassword =
                                if (message == "password updated") "" else it.confirmPassword,
                        )
                    }
                },
                onFailure = { error ->
                    _state.update {
                        it.copy(
                            busy = false,
                            passwordStatus = "Change password failed: ${error.message}",
                        )
                    }
                },
            )
        }
    }

    fun onLogSessionsChange(value: String) {
        val sessions = value.trim().filter { it.isDigit() }.ifBlank { "3" }
        _state.update { it.copy(logSessions = sessions) }
        prefs.edit().putString(KEY_LOG_SESSIONS, sessions).apply()
    }

    fun setLogControls(enabled: Boolean) {
        _state.update { it.copy(logControls = enabled) }
        prefs.edit().putBoolean(KEY_LOG_CONTROLS, enabled).apply()
        lastLoggedSourcePad = null
        ClientFileLog.append(
            if (enabled) "ctrl: logging enabled" else "ctrl: logging disabled",
        )
    }

    fun setLogConnections(enabled: Boolean) {
        _state.update { it.copy(logConnections = enabled) }
        prefs.edit().putBoolean(KEY_LOG_CONNECTIONS, enabled).apply()
        ClientFileLog.logConnections = enabled
        ClientFileLog.append(
            if (enabled) "conn: logging enabled" else "conn: logging disabled",
        )
    }

    /** Append a controls-debug line when Settings → Debug → Log controls is on. */
    private fun logControl(message: String) {
        if (!_state.value.logControls) return
        ClientFileLog.append("ctrl: $message")
    }

    private fun formatPad(pad: ControllerState): String =
        "buttons=0x${pad.buttons.toString(16)}" +
            " sticks=${pad.leftX},${pad.leftY}/${pad.rightX},${pad.rightY}" +
            " triggers=${pad.leftTrigger}/${pad.rightTrigger}"

    private fun logControlPad(source: String, pad: ControllerState, extra: String = "") {
        if (!_state.value.logControls) return
        val prev = lastLoggedSourcePad
        if (prev != null && pad.sameControlsAs(prev)) return
        lastLoggedSourcePad = pad
        val suffix = if (extra.isEmpty()) "" else " $extra"
        ClientFileLog.append("ctrl: $source → ${formatPad(pad)}$suffix")
    }

    fun sendLogsToHost() {
        if (_state.value.busy) return
        val snap = _state.value
        val primary = snap.host.trim()
        if (primary.isEmpty()) {
            _state.update { it.copy(logSendStatus = "Set a Host IP on the Client tab first.") }
            return
        }
        val alt = snap.altHost.trim()
        if (alt.isNotEmpty() && !HostAddresses.looksLikeIp(alt)) {
            _state.update {
                it.copy(logSendStatus = "Alt IP must look like an IP address, or leave it blank.")
            }
            return
        }
        val candidates = HostAddresses.connectCandidates(primary, alt)
        val controlPort = snap.controlPort.toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT
        val sessions = (snap.logSessions.toIntOrNull() ?: 3).coerceIn(1, 20)
        val username = sessionUsernameOrNull() ?: "android-client"
        _state.update { it.copy(busy = true, logSendStatus = "Sending logs…") }
        ClientFileLog.append("Send logs requested ($sessions session(s)) → $primary:$controlPort")
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    val text = ClientFileLog.extractLastSessions(sessions)
                    require(text.isNotEmpty()) { "client log is empty" }
                    var lastError: Throwable? = null
                    for ((index, host) in candidates.withIndex()) {
                        try {
                            return@runCatching ControlConnection(host, controlPort).use { conn ->
                                conn.connect()
                                conn.send(PacketCodec.clientLogBundle(username, sessions, text))
                                when (val reply = conn.receive()) {
                                    is IncomingPacket.Error -> reply.value.message
                                    else -> "unexpected reply from host"
                                }
                            }
                        } catch (err: Throwable) {
                            lastError = err
                            if (index >= candidates.lastIndex ||
                                !HostAddresses.isReachabilityFailure(err)
                            ) {
                                throw err
                            }
                        }
                    }
                    throw lastError ?: IllegalStateException("Send logs failed")
                }
            }
            result.fold(
                onSuccess = { message ->
                    ClientFileLog.append("Send logs ok: $message")
                    _state.update {
                        it.copy(busy = false, logSendStatus = message, status = message)
                    }
                },
                onFailure = { error ->
                    val msg = "Send logs failed: ${error.message}"
                    ClientFileLog.append(msg)
                    _state.update {
                        it.copy(busy = false, logSendStatus = msg, status = msg)
                    }
                },
            )
        }
    }

    fun onFilterChange(value: String) {
        _state.update { state ->
            val filter = value.trim().lowercase()
            val expanded = if (filter.isEmpty()) {
                state.expandedSystems
            } else {
                // Auto-expand systems (and Recents) that still have matches under the filter.
                val matched = state.games.filter {
                    it.title().lowercase().contains(filter) ||
                        it.version.lowercase().contains(filter) ||
                        it.systemName.lowercase().contains(filter) ||
                        it.systemKey.lowercase().contains(filter)
                }
                val systems = matched
                    .map { it.systemName.ifBlank { it.systemKey.ifBlank { "Other" } } }
                    .toMutableSet()
                val recentHit = state.recentGameIds.any { id ->
                    matched.any { it.id == id }
                }
                if (recentHit) {
                    systems.add(RECENT_GROUP)
                }
                systems
            }
            state.copy(filter = value, expandedSystems = expanded)
        }
    }

    fun toggleSystemExpanded(systemName: String) {
        _state.update { state ->
            val next = state.expandedSystems.toMutableSet()
            if (!next.add(systemName)) next.remove(systemName)
            state.copy(expandedSystems = next)
        }
    }

    fun setReceiveVideo(value: Boolean) = _state.update { it.copy(receiveVideo = value) }

    fun setReceiveAudio(value: Boolean) {
        _state.update { it.copy(receiveAudio = value) }
        val active = session
        if (active != null) {
            viewModelScope.launch(Dispatchers.IO) {
                runCatching { active.setReceiveAudio(value) }
                    .onFailure { err ->
                        ClientFileLog.conn("receive audio toggle failed: ${err.message ?: err}")
                    }
                _state.update {
                    it.copy(
                        status = if (value) "Audio on." else "Audio muted.",
                    )
                }
            }
        }
    }

    fun setStreamQuality(tier: MediaQualityTier) {
        _state.update { it.copy(streamQuality = tier) }
        prefs.edit().putInt(KEY_STREAM_QUALITY, tier.id).apply()
        applyStreamPrefsToSession()
    }

    fun setStreamSize(size: MediaStreamSize) {
        _state.update { it.copy(streamSize = size) }
        prefs.edit().putInt(KEY_STREAM_SIZE, size.id).apply()
        applyStreamPrefsToSession()
    }

    fun setStreamFeel(feel: MediaStreamFeel) {
        _state.update { it.copy(streamFeel = feel) }
        prefs.edit().putInt(KEY_STREAM_FEEL, feel.id).apply()
        applyStreamPrefsToSession()
    }

    /** Drawer open during play → pause (unless control editing relaxed it). */
    fun onPlayMenuOpened() {
        menuDrawerOpen = true
        logControl("menuDrawerOpen=true (pads muted for UDP)")
        clearRemotedKeys()
        clearKeyboardDpad()
        menuHatUp = false
        menuHatDown = false
        gamepadTracker.reset()
        latestPad = ControllerState()
        syncMenuPause()
    }

    /** Drawer closed → unpause (unless drawer re-opens before this applies). */
    fun onPlayMenuClosed() {
        menuDrawerOpen = false
        logControl("menuDrawerOpen=false (pads unmuted)")
        menuHatUp = false
        menuHatDown = false
        clearRemotedKeys()
        clearKeyboardDpad()
        syncMenuPause()
    }

    /**
     * Absolute pause from UI truth: drawer open pauses; control editing is the only
     * exception (game stays live under the editor). Never toggles / inverts.
     *
     * Only pause once the client has decoded at least one video frame — early drawer
     * open during stream init must not send EmulatorControl pause (Ryujinx F5 is a
     * toggle; On→Off during boot can leave the emu stuck paused and block cutover).
     *
     * Debounced: a same-frame Open→Closed (scrim eating the menu press, effect restart,
     * etc.) must not send pause=on then pause=off — that pair also desyncs F5.
     */
    private fun syncMenuPause() {
        if (!_state.value.playing) {
            menuPauseJob?.cancel()
            menuPauseJob = null
            lastSentMenuPause = null
            return
        }
        menuPauseJob?.cancel()
        menuPauseJob = viewModelScope.launch(Dispatchers.IO) {
            delay(MENU_PAUSE_DEBOUNCE_MS)
            if (!_state.value.playing) return@launch
            val hasFrames = session?.videoPlayer?.hasDecodedFrames() == true
            val wantPaused =
                menuDrawerOpen && !_state.value.overlayEditing && hasFrames
            // Initial Closed while playing / drawer open before first frame: do not poke.
            if (lastSentMenuPause == null && !wantPaused) return@launch
            if (lastSentMenuPause == wantPaused) return@launch
            pushEmulatorControls(
                pause = wantPaused,
                force = true,
            )
        }
    }

    /** OSK needs an unpaused emulator; bypass debounce so the dialog is not racing F5. */
    private fun ensureUnpausedForKeyboard() {
        if (!menuDrawerOpen && lastSentMenuPause != true) return
        menuDrawerOpen = false
        menuPauseJob?.cancel()
        menuPauseJob = null
        if (lastSentMenuPause != true) {
            lastSentMenuPause = false
            _state.update { it.copy(paused = false) }
            return
        }
        pushEmulatorControls(
            pause = false,
            force = true,
        )
    }

    /**
     * Play-menu pause switch (same absolute On/Off model as fast-forward).
     * Failsafe when menu F5 desyncs: flip Pause to match what you want the game to do.
     * Pause On is ignored until at least one video frame has been decoded.
     * @param force when true, re-send even if UI already matches (host cache may have drifted).
     */
    fun setPaused(enabled: Boolean, force: Boolean = false) {
        if (!_state.value.playing) return
        if (enabled && session?.videoPlayer?.hasDecodedFrames() != true) return
        if (!force && _state.value.paused == enabled) return
        menuPauseJob?.cancel()
        menuPauseJob = null
        pushEmulatorControls(pause = enabled, force = force)
    }

    /**
     * Play-menu / switch latch. Turns FF on and leaves it alone until turned off.
     * Hold input is separate — releasing R2 must not clear this latch.
     */
    fun setFastForward(enabled: Boolean) {
        if (!_state.value.playing) return
        ffMenuLatched = enabled
        _state.update { it.copy(fastForward = enabled) }
        publishEffectiveFastForward()
    }

    /**
     * Hold-to-FF from overlay pad, remapped L2/R2, or keyboard F.
     * Ignored until [ffInputArmed] so stream startup cannot poke the host.
     */
    fun setFastForwardHold(held: Boolean) {
        if (!_state.value.playing) return
        if (!ffInputArmed) {
            if (!held) {
                ffHoldPressed = false
            }
            return
        }
        if (ffHoldPressed == held) return
        ffHoldPressed = held
        publishEffectiveFastForward()
    }

    private fun effectiveFastForward(): Boolean = ffMenuLatched || ffHoldPressed

    /**
     * Send EmulatorControl FF only when latch∨hold changes. No force, no Off retries —
     * those re-cycled Ryujinx F1 and left Custom@200% stuck while UI showed Off.
     */
    private fun publishEffectiveFastForward() {
        if (!_state.value.playing) return
        val want = effectiveFastForward()
        if (lastSentFf == want) return
        scheduleFastForwardSend()
    }

    /** One-shot DS screen swap via EmulatorControl action (melonDS F6). */
    fun triggerScreenSwap() {
        if (!_state.value.playing) return
        val active = session ?: return
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                active.sendEmulatorControl(action = EmulatorControlAction.ScreenSwap)
            }.onFailure { err ->
                ClientFileLog.append(
                    "EmulatorControl screen_swap failed: ${err.javaClass.simpleName}: ${err.message}",
                )
            }
        }
        ClientFileLog.append("emuControl action=ScreenSwap")
    }

    /**
     * Single owner for desired pause → host EmulatorControlPlane.
     * Fast-forward is owned only by [setFastForward] / [setFastForwardHold].
     * @param pause null = leave Unchanged on the wire; non-null updates UI + sends On/Off
     * @param force ask host to re-apply pause even if its cache already matches
     */
    private fun pushEmulatorControls(
        pause: Boolean? = null,
        force: Boolean = false,
    ) {
        if (!_state.value.playing) return
        val active = session ?: return
        if (pause != null) {
            lastSentMenuPause = pause
            _state.update { it.copy(paused = pause) }
        }
        // Remoted P/Space must not fight absolute pause intents on the same session.
        clearRemotedKeys()
        val pauseWire = when (pause) {
            true -> EmulatorControlState.On
            false -> EmulatorControlState.Off
            null -> EmulatorControlState.Unchanged
        }
        if (pauseWire == EmulatorControlState.Unchanged) {
            return
        }
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                active.sendEmulatorControl(
                    pause = pauseWire,
                    force = force,
                )
            }.onFailure { err ->
                ClientFileLog.append(
                    "EmulatorControl send failed: ${err.javaClass.simpleName}: ${err.message}",
                )
            }
        }
        ClientFileLog.append(
            "emuControl push pause=${pause ?: "-"} ff=- force=$force",
        )
    }

    /**
     * Desktop "Resync A/V" fallback: restart Opus so audio meets the live video edge.
     * Rate-limited to once per 15s (same as desktop client). Video is not touched.
     * Does not touch pause or FF.
     */
    fun resyncAv() {
        if (!_state.value.playing) {
            _state.update { it.copy(status = "Join a session before resyncing A/V.") }
            return
        }
        viewModelScope.launch(Dispatchers.IO) {
            val status = when (tryResyncAudio(manual = true)) {
                1 -> "Realigned audio to video."
                0 -> "A/V resync cooling down… try again shortly."
                else -> "A/V resync failed (no audio stream?)."
            }
            _state.update { it.copy(status = status) }
        }
    }

    /**
     * Same audio-only restart as the Resync button. Call only from a background
     * coroutine — never inline on the heartbeat loop (that blocked and felt like
     * a multi-second game freeze).
     * @return 1 ok, 0 cooling down, -1 failed
     */
    private fun tryResyncAudio(manual: Boolean): Int {
        val now = System.currentTimeMillis()
        if (lastAvResyncAtMs > 0L && now - lastAvResyncAtMs < AV_RESYNC_MIN_INTERVAL_MS) {
            return 0
        }
        val ok = runCatching { session?.resyncAudio() == true }.getOrDefault(false)
        if (!ok) {
            return -1
        }
        lastAvResyncAtMs = now
        zeroFrameStreak = 0
        audioRealignAfterVideoStall = false
        ClientFileLog.append(
            if (manual) "A/V resync: audio restarted"
            else "A/V resync (auto): audio restarted",
        )
        return 1
    }

    /**
     * After a video stall (≥3 zero-frame heartbeats), fire the same async audio
     * restart as the Resync button when frames return — never block heartbeat.
     * Does not touch pause or FF.
     */
    private fun noteHeartbeatFrames(active: JoinedPlaySession, framesDelta: Int) {
        val video = active.videoPlayer ?: return
        if (active.audioPlayer == null) return
        if (video.hasDecodedFrames()) {
            if (!everDecodedVideoFrames) {
                everDecodedVideoFrames = true
                // Client + host both start Off; arm hold input only after video is live.
                ffInputArmed = true
                lastSentFf = false
                ClientFileLog.append("emuControl FF armed (hold+latch); default off")
            }
        }
        if (!everDecodedVideoFrames) return

        if (framesDelta == 0) {
            ++zeroFrameStreak
            if (zeroFrameStreak >= AV_STALL_ZERO_FRAME_HEARTBEATS) {
                audioRealignAfterVideoStall = true
            }
            return
        }
        zeroFrameStreak = 0
        if (!audioRealignAfterVideoStall) return
        audioRealignAfterVideoStall = false
        viewModelScope.launch(Dispatchers.IO) {
            if (tryResyncAudio(manual = false) == 1) {
                _state.update {
                    it.copy(status = "Video recovered; restarted audio to match (lip-sync).")
                }
            }
        }
    }

    private fun resetAvStallState() {
        zeroFrameStreak = 0
        audioRealignAfterVideoStall = false
        everDecodedVideoFrames = false
        lastAvResyncAtMs = 0L
    }

    private fun scheduleFastForwardSend() {
        ffJob?.cancel()
        ffJob = viewModelScope.launch(Dispatchers.IO) {
            delay(FF_COALESCE_MS)
            if (!_state.value.playing) return@launch
            val latest = effectiveFastForward()
            if (lastSentFf == latest) return@launch
            val elapsed = System.currentTimeMillis() - lastFfSendAtMs
            if (lastFfSendAtMs > 0L && elapsed < FF_MIN_INTERVAL_MS) {
                delay(FF_MIN_INTERVAL_MS - elapsed)
            }
            if (!_state.value.playing) return@launch
            val finalWant = effectiveFastForward()
            if (lastSentFf == finalWant) return@launch
            lastSentFf = finalWant
            lastFfSendAtMs = System.currentTimeMillis()
            runCatching {
                session?.sendEmulatorControl(
                    fastForward = if (finalWant) EmulatorControlState.On else EmulatorControlState.Off,
                    force = false,
                )
            }.onFailure { err ->
                ClientFileLog.append(
                    "EmulatorControl FF send failed: ${err.javaClass.simpleName}: ${err.message}",
                )
                lastSentFf = null
            }
            ClientFileLog.append(
                "emuControl push pause=- ff=$finalWant force=false " +
                    "(latch=$ffMenuLatched hold=$ffHoldPressed)",
            )
        }
    }

    private fun resetFastForwardSendState() {
        ffJob?.cancel()
        ffJob = null
        ffMenuLatched = false
        ffHoldPressed = false
        ffInputArmed = false
        lastSentFf = null
        lastFfSendAtMs = 0L
    }

    fun connect() {
        val snap = _state.value
        val primary = snap.host.trim()
        val altDraft = snap.altHost.trim()
        if (primary.isBlank()) {
            _state.update { it.copy(status = "Host IP is required.") }
            return
        }
        if (altDraft.isNotEmpty() && !HostAddresses.looksLikeIp(altDraft)) {
            _state.update {
                it.copy(status = "Alt IP must look like an IP address (e.g. 10.6.0.2), or leave it blank.")
            }
            return
        }
        val candidates = HostAddresses.connectCandidates(primary, altDraft)
        val port = snap.controlPort.toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT
        val sessionUser = sessionUsernameOrNull()
        if (sessionUser == null && snap.password.isNotEmpty()) {
            _state.update {
                it.copy(
                    status = "Set a Profile username before connecting with a password.",
                    section = NavSection.Profile,
                    passwordStatus = "Username required — blank falls through to a throwaway “android” save.",
                )
            }
            return
        }
        // Persist primary host/ports only. Alt is owned by onAltHostChange.
        // Username is owned by onUsernameChange — never overwrite with a blank field.
        prefs.edit()
            .putString(KEY_HOST, primary)
            .putString(KEY_CONTROL_PORT, snap.controlPort)
            .putString(KEY_INPUT_PORT, snap.inputPort)
            .apply()

        viewModelScope.launch {
            _state.update { it.copy(busy = true, status = "Fetching catalog from $primary:$port…") }
            clearLobbyPresence()
            val username = sessionUser
            val password = snap.password
            runCatching {
                withContext(Dispatchers.IO) {
                    val knownRev = snap.catalogRevision
                    val knownGames = snap.catalogCacheGames.ifEmpty { snap.games }
                    var lastError: Throwable? = null
                    for ((index, host) in candidates.withIndex()) {
                        if (index > 0) {
                            withContext(Dispatchers.Main) {
                                _state.update {
                                    it.copy(status = "Host IP unreachable; trying Alt IP $host:$port…")
                                }
                            }
                            ClientFileLog.conn("connect fallback primary=$primary → alt=$host")
                        }
                        try {
                            val result = if (password.isNotEmpty() && username != null) {
                                CatalogFetcher.fetchAndHoldPresence(
                                    host,
                                    port,
                                    username,
                                    password,
                                    knownRevision = knownRev,
                                    knownGames = knownGames,
                                    knownBlocksRevision = snap.blocksRevision,
                                    knownBlockedIds = snap.blockedGameIds,
                                )
                            } else {
                                CatalogFetcher.fetch(
                                    host,
                                    port,
                                    knownRevision = knownRev,
                                    knownGames = knownGames,
                                ) to null
                            }
                            return@withContext Triple(host, result.first, result.second)
                        } catch (err: Throwable) {
                            lastError = err
                            val canFallback = index < candidates.lastIndex &&
                                HostAddresses.isReachabilityFailure(err)
                            if (!canFallback) {
                                throw err
                            }
                            ClientFileLog.conn(
                                "connect failed $host:$port (${err.message ?: err}); will try Alt IP",
                            )
                        }
                    }
                    throw lastError ?: IllegalStateException("Connect failed")
                }
            }.onSuccess { (host, catalog, presence) ->
                lobbyPresence = presence
                ClientFileLog.append("Catalog loaded: ${catalog.games.size} games from $host:$port")
                val recentIds = loadRecentGameIds(host, snap.controlPort)
                val expanded = if (recentIds.any { id -> catalog.games.any { it.id == id } }) {
                    setOf(RECENT_GROUP)
                } else {
                    emptySet()
                }
                val presenceNote = if (presence != null) {
                    " Connected on host."
                } else {
                    " (enter password to register Connected on host)."
                }
                val viaAlt = !host.equals(primary, ignoreCase = true)
                _state.update {
                    it.copy(
                        busy = false,
                        connected = true,
                        // Host IP / Alt IP prefs stay as entered; art/join use reachable host.
                        section = NavSection.Games,
                        games = catalog.games,
                        catalogRevision = catalog.catalogRevision,
                        catalogCacheGames = catalog.cacheGames,
                        blocksRevision = catalog.blocksRevision,
                        blockedGameIds = catalog.blockedGameIds,
                        recentGameIds = recentIds,
                        expandedSystems = expanded,
                        artByAssetKey = emptyMap(),
                        status = "Loaded ${catalog.games.size} games from $host" +
                            (if (viaAlt) " (Alt IP)" else "") +
                            " (rev ${catalog.catalogRevision}).$presenceNote",
                        controlsSyncReady = profileUsernameOrNull() != null && presence != null,
                    )
                }
                startArtPrefetch(catalog.games, host, port, catalog.catalogRevision)
            }.onFailure { err ->
                clearLobbyPresence()
                _state.update {
                    it.copy(
                        busy = false,
                        connected = false,
                        status = "Connect failed: ${err.message ?: err}",
                    )
                }
            }
        }
    }

    private fun startArtPrefetch(
        games: List<GameInfo>,
        host: String,
        controlPort: Int,
        catalogRevision: Long,
    ) {
        artJob?.cancel()
        val cacheDir = File(getApplication<Application>().cacheDir, "archstreamer")
        artJob = viewModelScope.launch(Dispatchers.IO) {
            ArtFetcher.prefetchAll(
                host = host,
                controlPort = controlPort,
                games = games,
                cacheDir = cacheDir,
                catalogRevision = catalogRevision,
                isActive = { isActive && _state.value.connected },
            ) { assetKey, bitmap ->
                _state.update { state ->
                    state.copy(artByAssetKey = state.artByAssetKey + (assetKey to bitmap))
                }
            }
        }
    }

    fun disconnect() {
        artJob?.cancel()
        artJob = null
        clearLobbyPresence()
        endSession()
        rehydrateUsernameFromPrefs()
        _state.update {
            it.copy(
                playing = false,
                connected = false,
                section = NavSection.Client,
                // Keep catalogRevision + catalogCacheGames so the next Connect can
                // take the host unchanged short-circuit without an empty UI.
                games = it.catalogCacheGames.ifEmpty { it.games },
                selectedGame = null,
                mediaHint = "",
                videoPlayer = null,
                softKeyboard = null,
                artByAssetKey = emptyMap(),
                expandedSystems = emptySet(),
                status = "Disconnected.",
            )
        }
    }

    fun startGame(game: GameInfo) {
        val snap = _state.value
        val primary = snap.host.trim()
        val altDraft = snap.altHost.trim()
        if (primary.isBlank()) {
            _state.update {
                it.copy(status = "Host IP is required.", section = NavSection.Client)
            }
            return
        }
        if (altDraft.isNotEmpty() && !HostAddresses.looksLikeIp(altDraft)) {
            _state.update {
                it.copy(
                    status = "Alt IP must look like an IP address (e.g. 10.6.0.2), or leave it blank.",
                    section = NavSection.Client,
                )
            }
            return
        }
        val candidates = HostAddresses.connectCandidates(primary, altDraft)
        val controlPort = snap.controlPort.toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT
        val inputPort = snap.inputPort.toIntOrNull() ?: Protocol.DEFAULT_INPUT_PORT
        val username = sessionUsernameOrNull()
        if (username == null) {
            _state.update {
                it.copy(
                    status = "Set a Profile username before joining — saves are keyed by that name.",
                    section = NavSection.Profile,
                    passwordStatus = "Username required (never join as the “android” placeholder).",
                )
            }
            return
        }
        val password = snap.password
        if (password.isEmpty()) {
            _state.update {
                it.copy(
                    status = "Enter your password on the Client tab.",
                    section = NavSection.Client,
                    passwordStatus = "Password required before joining.",
                )
            }
            return
        }

        viewModelScope.launch {
            _state.update {
                it.copy(
                    busy = true,
                    status = "Starting ${game.title()}…",
                    selectedGame = game,
                    softKeyboard = null,
                )
            }
            runCatching {
                withContext(Dispatchers.IO) {
                    clearLobbyPresence()
                    endSessionLocked()
                    val preferPhysical = snap.usePhysicalController
                    val pads = PhysicalGamepad.connectedPads()
                    val usePad = preferPhysical && pads.isNotEmpty()
                    val pad = pads.firstOrNull()
                    var lastError: Throwable? = null
                    for ((index, host) in candidates.withIndex()) {
                        if (index > 0) {
                            withContext(Dispatchers.Main) {
                                _state.update {
                                    it.copy(status = "Host IP unreachable; joining via Alt IP $host…")
                                }
                            }
                            ClientFileLog.conn("join fallback primary=$primary → alt=$host")
                        }
                        try {
                            val joined = SessionJoiner.join(
                                host,
                                controlPort,
                                inputPort,
                                username,
                                password,
                                game,
                                receiveAudio = snap.receiveAudio,
                                displayLayout = DisplayLayoutPreference.Landscape.id,
                                controllerName = if (usePad) {
                                    pad?.name ?: "Android Gamepad"
                                } else {
                                    "Android Touch Pad"
                                },
                                controllerGuid = if (usePad) {
                                    pad?.descriptor ?: "android-pad-0"
                                } else {
                                    "android-touch-0"
                                },
                                knownCatalogRevision = snap.catalogRevision,
                                knownBlocksRevision = snap.blocksRevision,
                                onPasswordChangeRequired = {
                                    val latch = java.util.concurrent.CountDownLatch(1)
                                    passwordChangeLatch = latch
                                    passwordChangeResult = ""
                                    _state.update {
                                        it.copy(
                                            forcePasswordChange = true,
                                            forcePasswordDraft = "",
                                            forcePasswordConfirm = "",
                                            passwordStatus = "Host requires a new password.",
                                        )
                                    }
                                    latch.await()
                                    passwordChangeLatch = null
                                    passwordChangeResult.also {
                                        require(it.isNotEmpty()) { "password change cancelled" }
                                    }
                                },
                            )
                            return@withContext host to joined
                        } catch (err: Throwable) {
                            lastError = err
                            val canFallback = index < candidates.lastIndex &&
                                HostAddresses.isReachabilityFailure(err)
                            if (!canFallback) throw err
                            ClientFileLog.conn(
                                "join failed $host:$controlPort (${err.message ?: err}); will try Alt IP",
                            )
                        }
                    }
                    throw lastError ?: IllegalStateException("Join failed")
                }
            }.onSuccess { (host, joined) ->
                session = joined
                applyStreamPrefsToSession()
                startInputLoop()
                startHeartbeatLoop()
                startControlLoop()
                joined.takePendingSoftKeyboard()?.let { showSoftKeyboard(it) }
                val media = joined.media
                sessionSystemKey = game.systemKey
                sessionFamily = OverlaySystemFamily.fromSystemKey(game.systemKey)
                val profile = profileFor(sessionFamily)
                val mapProfile = mapProfileFor(sessionFamily)
                val recentIds = rememberRecentGame(game.id, host, snap.controlPort)
                _state.update {
                    it.copy(
                        busy = false,
                        playing = true,
                        host = host,
                        recentGameIds = recentIds,
                        videoPlayer = joined.videoPlayer,
                        padLayout = profile.resolveLayout(game.systemKey),
                        overlayOpacity = profile.clampedOpacity(),
                        swapNw = mapProfile.swapNw,
                        swapSe = mapProfile.swapSe,
                        overlayItems = profile.resolveItems(
                            game.systemKey,
                            _state.value.overlayOrientation,
                        ),
                        overlayEditing = false,
                        editingOverlayFamily = sessionFamily,
                        editingOverlayProfile = profile,
                        editingMapProfile = mapProfile,
                        reconnectHintGameId = null,
                        playlistDiscs = game.playlistDiscs,
                        discIndex = 0,
                        discStatus = if (game.playlistDiscs.size >= 2) {
                            "Disc 1 / ${game.playlistDiscs.size}"
                        } else {
                            ""
                        },
                        linkCapable = systemSupportsLink(game.systemKey),
                        linkPeerDraft = "",
                        linkStatus = "",
                        linkStatusKind = null,
                        status = "Playing ${game.title()}",
                        controlsSyncReady = profileUsernameOrNull() != null,
                        paused = false,
                        fastForward = false,
                        mediaHint = run {
                            val video = joined.videoPlayer
                            val audio = joined.audioPlayer
                            when {
                                video != null && audio != null ->
                                    "Video :${video.port} · Audio :${audio.port}"
                                video != null ->
                                    "Video UDP :${video.port} (no audio URI — check Receive audio)"
                                media != null ->
                                    "No video bind — ${media.videoUri}"
                                else ->
                                    "No MediaEndpoint from host"
                            }
                        },
                    )
                }
                // Grace reconnect / fresh join: pause Off only. FF stays default Off on
                // both sides — do not poke F1 during stream init (hold arms after first frame).
                menuDrawerOpen = false
                lastSentMenuPause = false
                resetFastForwardSendState()
                viewModelScope.launch(Dispatchers.IO) {
                    pushEmulatorControls(pause = false, force = false)
                }
            }.onFailure { err ->
                val msg = err.message ?: err.toString()
                val canReconnect = msg.contains("already has an active session", ignoreCase = true) ||
                    msg.contains("reconnect", ignoreCase = true) ||
                    msg.contains("reserving", ignoreCase = true)
                _state.update {
                    it.copy(
                        busy = false,
                        playing = false,
                        section = NavSection.Games,
                        videoPlayer = null,
                        softKeyboard = null,
                        padLayout = PadLayout.Standard,
                        reconnectHintGameId = if (canReconnect) game.id else null,
                        status = if (canReconnect) {
                            "Host still has your seat for ~60s. Tap ${game.title()} again to reconnect " +
                                "(same username). Leave session ends it immediately."
                        } else {
                            "Start failed: $msg"
                        },
                    )
                }
            }
        }
    }

    fun onPadState(state: ControllerState) {
        val snap = _state.value
        if (snap.physicalInputActive) {
            if (state.buttons != 0 ||
                state.leftTrigger != 0 ||
                state.rightTrigger != 0 ||
                state.leftX.toInt() != 0 ||
                state.leftY.toInt() != 0
            ) {
                logControl(
                    "overlay ignored (physicalInputActive) ${formatPad(state)} " +
                        "pad=${snap.physicalPadLabel.ifBlank { "?" }}",
                )
            }
            return
        }
        val swapped = ControllerState.applyFaceButtonSwaps(state, snap.swapNw, snap.swapSe)
        latestPad = swapped
        logControlPad(
            "overlay",
            swapped,
            "swapNw=${snap.swapNw} swapSe=${snap.swapSe} menuOpen=$menuDrawerOpen",
        )
    }

    /** Remoted DS stylus; coords are normalized 0..65535 within the bottom screen. */
    fun onDsTouch(normX: Int, normY: Int, pressed: Boolean) {
        if (!_state.value.playing || session == null) return
        // Cap so a stuck gesture cannot grow without bound.
        while (pendingDsTouches.size >= 32) {
            pendingDsTouches.poll()
        }
        pendingDsTouches.add(Triple(normX, normY, pressed))
    }

    fun leavePlay() {
        menuDrawerOpen = false
        lastSentMenuPause = null
        resetAvStallState()
        pendingDsTouches.clear()
        clearRemotedKeys()
        clearKeyboardDpad()
        latestPad = ControllerState()
        endSession()
        _state.update {
            it.copy(
                playing = false,
                section = NavSection.Games,
                selectedGame = null,
                mediaHint = "",
                videoPlayer = null,
                softKeyboard = null,
                dsScreenLayout = null,
                padLayout = PadLayout.Standard,
                overlayItems = OverlayPresets.forLayout(PadLayout.Standard),
                overlayEditing = false,
                reconnectHintGameId = null,
                fastForward = false,
                paused = false,
                playlistDiscs = emptyList(),
                discIndex = 0,
                discStatus = "",
                linkCapable = false,
                linkPeerDraft = "",
                linkStatus = "",
                linkStatusKind = null,
                status = "Left session.",
            )
        }
        refreshPhysicalPads()
    }

    fun onLinkPeerChange(value: String) {
        _state.update { it.copy(linkPeerDraft = value) }
    }

    fun requestDiscSetIndex(index: Int) {
        val game = _state.value.selectedGame ?: return
        val discs = _state.value.playlistDiscs
        if (!_state.value.playing || discs.size < 2) return
        val clamped = index.coerceIn(0, discs.lastIndex)
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                session?.sendDiscControl(game.id, DiscControlAction.SetIndex, clamped)
            }.onFailure { err ->
                _state.update {
                    it.copy(discStatus = "Disc failed: ${err.message ?: err}")
                }
            }
        }
    }

    fun requestDiscNext() {
        val game = _state.value.selectedGame ?: return
        if (!_state.value.playing || _state.value.playlistDiscs.size < 2) return
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                session?.sendDiscControl(game.id, DiscControlAction.Next)
            }.onFailure { err ->
                _state.update {
                    it.copy(discStatus = "Disc failed: ${err.message ?: err}")
                }
            }
        }
    }

    fun requestDiscPrev() {
        val game = _state.value.selectedGame ?: return
        if (!_state.value.playing || _state.value.playlistDiscs.size < 2) return
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                session?.sendDiscControl(game.id, DiscControlAction.Prev)
            }.onFailure { err ->
                _state.update {
                    it.copy(discStatus = "Disc failed: ${err.message ?: err}")
                }
            }
        }
    }

    fun requestLink() {
        val snap = _state.value
        val game = snap.selectedGame ?: return
        if (!snap.playing || !snap.linkCapable) return
        val peer = snap.linkPeerDraft.trim()
        if (peer.isEmpty()) {
            _state.update { it.copy(linkStatus = "Enter the other player's username.") }
            return
        }
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                session?.sendLinkRequest(game.id, peer, LinkAction.Request)
            }.onFailure { err ->
                _state.update {
                    it.copy(linkStatus = "Link failed: ${err.message ?: err}")
                }
            }
        }
    }

    fun cancelLink() {
        val snap = _state.value
        val game = snap.selectedGame ?: return
        if (!snap.playing || !snap.linkCapable) return
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                session?.sendLinkRequest(game.id, "", LinkAction.Cancel)
            }.onFailure { err ->
                _state.update {
                    it.copy(linkStatus = "Link cancel failed: ${err.message ?: err}")
                }
            }
        }
    }

    /** Manual escape hatch (request_id=0) — same as desktop Controls pad OSK. */
    fun openManualSoftKeyboard() {
        if (session == null || !_state.value.playing) {
            _state.update { it.copy(status = "Start a session before opening the soft keyboard.") }
            return
        }
        ensureUnpausedForKeyboard()
        showSoftKeyboard(
            SoftKeyboardRequest(
                requestId = 0L,
                prompt = "Software Keyboard",
                initialText = "",
                maxLength = 12,
            ),
        )
    }

    fun submitSoftKeyboard(text: String) {
        val request = _state.value.softKeyboard ?: return
        val maxLen = request.maxLength.coerceIn(1, 64)
        val cleaned = sanitizeSoftKeyboardText(text, maxLen).trim()
        _state.update { it.copy(softKeyboard = null) }
        if (cleaned.isEmpty()) {
            cancelSoftKeyboard()
            return
        }
        val response = SoftKeyboardResponse(
            requestId = request.requestId,
            accepted = true,
            text = cleaned,
        )
        viewModelScope.launch(Dispatchers.IO) {
            runCatching { session?.sendSoftKeyboardResponse(response) }
                .onFailure { err ->
                    _state.update {
                        it.copy(status = "Soft keyboard send failed: ${err.message ?: err}")
                    }
                }
        }
        _state.update {
            it.copy(status = "Sent soft keyboard text (${cleaned.length} chars).")
        }
    }

    fun cancelSoftKeyboard() {
        val request = _state.value.softKeyboard
        _state.update { it.copy(softKeyboard = null) }
        if (request == null) return
        // Host-driven cancel: empty + not accepted. Watcher must not invent text;
        // if the game dialog is still Ready it publishes another SoftKeyboardRequest.
        if (request.requestId != 0L) {
            val response = SoftKeyboardResponse(
                requestId = request.requestId,
                accepted = false,
                text = "",
            )
            viewModelScope.launch(Dispatchers.IO) {
                runCatching { session?.sendSoftKeyboardResponse(response) }
            }
        }
    }

    private fun showSoftKeyboard(request: SoftKeyboardRequest) {
        val current = _state.value.softKeyboard
        // Avoid rebuilding (and wiping typed text) for a duplicate host poll of the same id.
        if (current != null && current.requestId == request.requestId && request.requestId != 0L) {
            return
        }
        ensureUnpausedForKeyboard()
        clearRemotedKeys()
        clearKeyboardDpad()
        _state.update {
            it.copy(
                softKeyboard = request,
                status = if (request.prompt.isBlank()) {
                    "Host requested software keyboard."
                } else {
                    "Host requested software keyboard: ${request.prompt}"
                },
            )
        }
    }

    private fun startInputLoop() {
        inputJob?.cancel()
        inputJob = viewModelScope.launch(Dispatchers.IO) {
            // Poll often so short taps are not missed; only spam UDP on change.
            // Idle keepalive ~25 Hz (desktop keeps ~250 Hz — fine on LAN PC, wasteful on phone).
            val pollMs = 4L
            val keepaliveMs = 40L
            val changeCopies = 3
            var lastSent = ControllerState()
            var haveLast = false
            var lastSendAtMs = 0L
            var lastSentKeys = -1
            var lastKeysSendAtMs = 0L
            while (isActive) {
                val active = session ?: break
                // Drain stylus first so press+release in one poll both go out.
                while (true) {
                    val touch = pendingDsTouches.poll() ?: break
                    runCatching {
                        active.sendTouch(touch.first, touch.second, touch.third)
                    }.onFailure { err ->
                        ClientFileLog.append(
                            "Touch send failed: ${err.javaClass.simpleName}: ${err.message}",
                        )
                    }
                }
                val pad = padForSend()
                val now = System.currentTimeMillis()
                val changed = !haveLast || !pad.sameControlsAs(lastSent)
                val dueKeepalive = haveLast && (now - lastSendAtMs) >= keepaliveMs
                if (changed || dueKeepalive || !haveLast) {
                    val copies = if (changed || !haveLast) changeCopies else 1
                    if (changed || !haveLast) {
                        logControl(
                            "udp pad ${formatPad(pad)} copies=$copies " +
                                "menuOpen=$menuDrawerOpen " +
                                "kbDpad=0x${keyboardDpadBits.get().toString(16)} " +
                                "physicalActive=${_state.value.physicalInputActive}",
                        )
                    }
                    val sentOk = runCatching {
                        active.sendControllerCopies(localPlayer = 0, state = pad, copies = copies)
                    }.onFailure { err ->
                        ClientFileLog.append(
                            "Input send failed: ${err.javaClass.simpleName}: ${err.message}",
                        )
                    }.isSuccess
                    // Only advance "last sent" on success so a failed edge is retried.
                    if (sentOk) {
                        lastSent = pad
                        haveLast = true
                        lastSendAtMs = now
                    } else if (changed || !haveLast) {
                        logControl("udp pad send FAILED ${formatPad(pad)}")
                    }
                }
                val keys =
                    if (menuDrawerOpen || _state.value.softKeyboard != null) {
                        0
                    } else {
                        remotedKeysHeld.get()
                    }
                val keysChanged = keys != lastSentKeys
                val keysKeepalive = lastSentKeys >= 0 && (now - lastKeysSendAtMs) >= keepaliveMs
                if (keysChanged || keysKeepalive || lastSentKeys < 0) {
                    if (keysChanged || lastSentKeys < 0) {
                        logControl(
                            "udp keys=0x${keys.toString(16)} " +
                                "menuOpen=$menuDrawerOpen osk=${_state.value.softKeyboard != null}",
                        )
                    }
                    val keysOk = runCatching {
                        active.sendKeyboard(keys)
                    }.onFailure { err ->
                        ClientFileLog.append(
                            "Keyboard send failed: ${err.javaClass.simpleName}: ${err.message}",
                        )
                    }.isSuccess
                    if (keysOk) {
                        lastSentKeys = keys
                        lastKeysSendAtMs = now
                    }
                }
                delay(pollMs)
            }
        }
    }

    private fun startHeartbeatLoop() {
        heartbeatJob?.cancel()
        resetAvStallState()
        heartbeatJob = viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                val active = session ?: break
                val frames = runCatching { active.sendHeartbeat() }.getOrElse { failed ->
                    failPlaySession(
                        message = "Connection lost (heartbeat): ${failed.message ?: failed}",
                        sendLeave = false,
                    )
                    return@launch
                }
                noteHeartbeatFrames(active, frames)
                // Drawer may have opened before the first decoded frame; apply pause once
                // frames exist so we still pause for a long-open menu after init.
                if (menuDrawerOpen &&
                    lastSentMenuPause != true &&
                    !_state.value.overlayEditing &&
                    active.videoPlayer?.hasDecodedFrames() == true
                ) {
                    syncMenuPause()
                }
                delay(1_000L)
            }
        }
    }

    private fun startControlLoop() {
        controlJob?.cancel()
        controlJob = viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                val packet = runCatching {
                    session?.tryReceiveControl(timeoutMs = 500)
                }.getOrElse { err ->
                    failPlaySession(
                        message = "Connection lost: ${err.message ?: err}. " +
                            "Tap the same game within ~60s to reconnect (same username).",
                        sendLeave = false,
                    )
                    return@launch
                }
                when (packet) {
                    is IncomingPacket.ControlsDb,
                    is IncomingPacket.ControlsDbAck,
                    -> {
                        val waiter = controlsSyncWaiter.getAndSet(null)
                        if (waiter != null) {
                            waiter.complete(packet)
                        }
                    }
                    is IncomingPacket.SoftKeyboard -> showSoftKeyboard(packet.value)
                    is IncomingPacket.DsScreens -> {
                        _state.update { it.copy(dsScreenLayout = packet.value) }
                    }
                    is IncomingPacket.DiscControl -> {
                        val response = packet.value
                        _state.update {
                            it.copy(
                                discIndex = response.discIndex.coerceAtLeast(0),
                                discStatus = if (response.ok) {
                                    response.message.ifBlank {
                                        "Disc ${response.discIndex + 1} / ${response.discCount}"
                                    }
                                } else {
                                    "Disc failed: ${response.message}"
                                },
                            )
                        }
                    }
                    is IncomingPacket.Link -> {
                        val response = packet.value
                        val prefix = when {
                            !response.ok -> "Link failed"
                            response.status == LinkStatus.Matched -> "Link matched"
                            response.status == LinkStatus.Pending -> "Link pending"
                            response.status == LinkStatus.Cancelled -> "Link cancelled"
                            else -> "Link"
                        }
                        _state.update {
                            it.copy(
                                linkStatusKind = response.status,
                                linkStatus = if (response.message.isNotBlank()) {
                                    "$prefix: ${response.message}"
                                } else {
                                    prefix
                                },
                            )
                        }
                    }
                    is IncomingPacket.VideoPending -> {
                        val ok = session?.beginVideoPending(packet.videoUri) == true
                        if (!ok) {
                            ClientFileLog.append(
                                "Video staging bind failed for ${packet.videoUri}",
                            )
                        }
                    }
                    is IncomingPacket.Media -> {
                        val promoted = session?.promoteVideo(packet.value)
                        if (promoted != null) {
                            _state.update {
                                it.copy(
                                    videoPlayer = promoted,
                                    mediaHint = "Video UDP :${promoted.port}",
                                )
                            }
                            // Staging Ready means frames exist; apply deferred drawer pause.
                            if (menuDrawerOpen) syncMenuPause()
                        }
                    }
                    is IncomingPacket.Ended -> {
                        val reason = packet.value.reason
                        failPlaySession(
                            message = "Session ended: $reason",
                            sendLeave = false,
                            reconnectHint = true,
                        )
                        return@launch
                    }
                    is IncomingPacket.Error -> {
                        _state.update {
                            it.copy(status = "Host error: ${packet.value.message}")
                        }
                    }
                    null -> {
                        // While idle on the control socket, check whether staged
                        // video has decoded frames and ACK the host cutover.
                        session?.pollVideoCutoverReady()
                    }
                    else -> Unit
                }
            }
        }
    }

    /**
     * Single teardown path for control/heartbeat/input death and SessionEnded.
     * Safe to call from multiple loops — only the first caller wins.
     */
    private fun failPlaySession(
        message: String,
        sendLeave: Boolean,
        reconnectHint: Boolean = true,
    ) {
        if (session == null && !_state.value.playing) return
        val gameId = _state.value.selectedGame?.id
        endSession(sendLeave = sendLeave)
        _state.update {
            it.copy(
                playing = false,
                section = NavSection.Games,
                mediaHint = "",
                videoPlayer = null,
                softKeyboard = null,
                playlistDiscs = emptyList(),
                discIndex = 0,
                discStatus = "",
                linkCapable = false,
                linkPeerDraft = "",
                linkStatus = "",
                linkStatusKind = null,
                reconnectHintGameId = if (reconnectHint) gameId else null,
                status = message,
            )
        }
    }

    private fun refreshControlsSyncReady() {
        val ready = profileUsernameOrNull() != null &&
            (
                (lobbyPresence?.isConnected() == true) ||
                    (_state.value.playing && session != null)
                )
        _state.update { it.copy(controlsSyncReady = ready) }
    }

    private fun clearLobbyPresence() {
        val held = lobbyPresence
        lobbyPresence = null
        if (held != null) {
            // Same pitfall as ClientSessionLeave: closing the TCP socket on the
            // main thread can throw NetworkOnMainThreadException, so the host
            // never sees FIN and Users stays "Connected".
            runBlocking {
                withContext(Dispatchers.IO) {
                    runCatching { held.close() }
                }
            }
        }
        refreshControlsSyncReady()
    }

    /**
     * Tear down the play session.
     *
     * [sendLeave] true (Leave button): send ClientSessionLeave on IO so the host
     * ends the seat immediately. false (TCP already dead / SessionEnded): skip
     * leave so a network drop keeps the configured reconnect hold.
     */
    private fun endSession(sendLeave: Boolean = true) {
        menuDrawerOpen = false
        lastSentMenuPause = null
        resetAvStallState()
        clearRemotedKeys()
        resetFastForwardSendState()
        val active = session
        session = null
        controlJob?.cancel()
        controlJob = null
        inputJob?.cancel()
        inputJob = null
        heartbeatJob?.cancel()
        heartbeatJob = null
        if (active == null) {
            latestPad = ControllerState()
            refreshControlsSyncReady()
            return
        }
        // Leave must not run on the main thread (NetworkOnMainThreadException
        // was aborting ClientSessionLeave with a null message, so the host only
        // saw TCP close and held the seat for reconnect).
        runBlocking {
            withContext(Dispatchers.IO) {
                if (sendLeave) {
                    val ok = runCatching { active.leave() }
                        .onFailure {
                            ClientFileLog.append(
                                "ClientSessionLeave failed: ${it.javaClass.simpleName}: ${it.message}",
                            )
                        }
                        .getOrDefault(false)
                    if (ok) {
                        // Let the leave frame flush before FIN so the host reads it.
                        delay(120)
                    }
                }
                runCatching { active.closeAfterLeave() }
            }
        }
        latestPad = ControllerState()
        refreshControlsSyncReady()
    }

    private fun endSessionLocked() {
        endSession(sendLeave = true)
    }

    override fun onCleared() {
        inputManager?.unregisterInputDeviceListener(inputDeviceListener)
        discoveryJob?.cancel()
        discoveryJob = null
        artJob?.cancel()
        artJob = null
        endSession(sendLeave = true)
        SessionKeepAliveService.stop(getApplication())
        super.onCleared()
    }

    companion object {
        const val RECENT_GROUP = "Recents"
        /** Local join placeholder — never used for SQL persistence or host sync. */
        const val PLACEHOLDER_USERNAME = "android"

        fun isProfileUsername(name: String): Boolean {
            val trimmed = name.trim()
            return trimmed.isNotEmpty() &&
                !trimmed.equals(PLACEHOLDER_USERNAME, ignoreCase = true)
        }

        private const val PREFS_NAME = "archstreamer_client"
        private const val KEY_HOST = "host"
        private const val KEY_ALT_HOST = "alt_host"
        private const val KEY_CONTROL_PORT = "control_port"
        private const val KEY_INPUT_PORT = "input_port"
        private const val KEY_USERNAME = "username"
        private const val KEY_STREAM_QUALITY = "stream_quality"
        private const val KEY_STREAM_SIZE = "stream_size"
        private const val KEY_STREAM_FEEL = "stream_feel"
        private const val KEY_DISCOVERY_SEEDS = "discovery_seeds"
        private const val KEY_LOG_SESSIONS = "log_sessions"
        private const val KEY_LOG_CONTROLS = "log_controls"
        private const val KEY_LOG_CONNECTIONS = "log_connections"
        private const val KEY_USE_PHYSICAL = "use_physical_controller"
        private const val KEY_REMOTE_SSH_HOST = "remote_ssh_host"
        private const val KEY_REMOTE_SSH_USER = "remote_ssh_user"
        private const val KEY_REMOTE_SSH_PORT = "remote_ssh_port"
        private const val KEY_REMOTE_DIRECTORY = "remote_directory"
        private const val KEY_REMOTE_ROM_ROOT = "remote_rom_root"
        private const val KEY_REMOTE_BINARY = "remote_binary"
        private const val KEY_REMOTE_START_SCRIPT = "remote_start_script"
        private const val KEY_REMOTE_GPU = "remote_gpu"
        private const val KEY_REMOTE_BASE_CONTROL = "remote_base_control"
        private const val KEY_REMOTE_BASE_INPUT = "remote_base_input"
        private const val KEY_REMOTE_TRACKED_CONTROL = "remote_tracked_control"
        private const val KEY_RECENT_PREFIX = "recent_games_"
        private const val MAX_DISCOVERY_SEEDS = 6
        private const val MAX_RECENT_GAMES = 8
        private const val HOST_EDIT_SUPPRESS_MS = 4_000L
        /** Wait for drawer settle; coalesce flash Open→Closed before poking the host. */
        private const val MENU_PAUSE_DEBOUNCE_MS = 250L
        /** Ignore FF button bounce before rate-limit clock. */
        private const val FF_COALESCE_MS = 80L
        /** Max one FF EmulatorControl edge per second (Ryujinx F1 cycle). */
        private const val FF_MIN_INTERVAL_MS = 1_000L
        /** Match desktop client_app A/V resync cooldown. */
        private const val AV_RESYNC_MIN_INTERVAL_MS = 15_000L
        /** Desktop: ≥3 consecutive zero-frame heartbeats arms audio realign. */
        private const val AV_STALL_ZERO_FRAME_HEARTBEATS = 3
    }

    private fun recentPrefsKey(host: String, controlPort: String): String {
        val h = host.trim().lowercase()
        val p = controlPort.trim().ifBlank { Protocol.DEFAULT_CONTROL_PORT.toString() }
        return "$KEY_RECENT_PREFIX${h}_$p"
    }

    private fun loadRecentGameIds(host: String, controlPort: String): List<String> {
        if (host.isBlank()) return emptyList()
        return prefs.getString(recentPrefsKey(host, controlPort), "")
            .orEmpty()
            .split(',')
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .take(MAX_RECENT_GAMES)
    }

    private fun rememberRecentGame(gameId: String, host: String, controlPort: String): List<String> {
        if (gameId.isBlank() || host.isBlank()) {
            return loadRecentGameIds(host, controlPort)
        }
        val next = loadRecentGameIds(host, controlPort).toMutableList()
        next.remove(gameId)
        next.add(0, gameId)
        val trimmed = next.take(MAX_RECENT_GAMES)
        prefs.edit()
            .putString(recentPrefsKey(host, controlPort), trimmed.joinToString(","))
            .apply()
        return trimmed
    }
}
