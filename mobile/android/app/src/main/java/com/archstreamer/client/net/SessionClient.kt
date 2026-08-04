package com.archstreamer.client.net

import com.archstreamer.client.media.MediaUris
import com.archstreamer.client.media.RtpOpusPlayer
import com.archstreamer.client.media.RtpVideoPlayer
import com.archstreamer.client.protocol.ControllerInfo
import com.archstreamer.client.protocol.ControllerState
import com.archstreamer.client.protocol.DiscControlAction
import com.archstreamer.client.protocol.DisplayLayoutPreference
import com.archstreamer.client.protocol.EmulatorControlState
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.GameSessionMode
import com.archstreamer.client.protocol.HostWelcome
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.LinkAction
import com.archstreamer.client.protocol.MediaEndpoint
import com.archstreamer.client.protocol.MediaQualityTier
import com.archstreamer.client.protocol.MediaStreamSize
import com.archstreamer.client.protocol.PacketCodec
import com.archstreamer.client.protocol.Protocol
import com.archstreamer.client.protocol.SeatAssignment
import com.archstreamer.client.protocol.SessionReady
import com.archstreamer.client.protocol.SessionStarting
import com.archstreamer.client.protocol.SoftKeyboardRequest
import com.archstreamer.client.protocol.SoftKeyboardResponse
import android.graphics.ImageFormat
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.view.Surface
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.atomic.AtomicReference

data class CatalogResult(
    val host: String,
    val controlPort: Int,
    val games: List<GameInfo>,
    val catalogRevision: Long,
)

/**
 * Matches ClientSessionService::begin — open TCP, ask for GameList, return catalog.
 * [fetch] closes afterward; [fetchAndHoldPresence] keeps the TCP for Users-tab Connected.
 */
object CatalogFetcher {
    fun fetch(
        host: String,
        controlPort: Int = Protocol.DEFAULT_CONTROL_PORT,
        knownRevision: Long = 0L,
    ): CatalogResult {
        ControlConnection(host, controlPort).use { conn ->
            conn.connect()
            return fetchOn(conn, host, controlPort, knownRevision)
        }
    }

    /**
     * Fetch catalog then announce LobbyPresence on the same socket.
     * Caller owns the returned connection until play starts or disconnect.
     */
    fun fetchAndHoldPresence(
        host: String,
        controlPort: Int,
        username: String,
        password: String,
        knownRevision: Long = 0L,
    ): Pair<CatalogResult, ControlConnection> {
        val conn = ControlConnection(host, controlPort)
        try {
            conn.connect()
            val catalog = fetchOn(conn, host, controlPort, knownRevision)
            conn.send(PacketCodec.lobbyPresence(username, password))
            when (val ack = conn.receive()) {
                is IncomingPacket.LobbyPresenceAck -> Unit
                is IncomingPacket.Error -> error(ack.value.message)
                else -> error("expected LobbyPresenceAck, got $ack")
            }
            return catalog to conn
        } catch (t: Throwable) {
            runCatching { conn.close() }
            throw t
        }
    }

    private fun fetchOn(
        conn: ControlConnection,
        host: String,
        controlPort: Int,
        knownRevision: Long,
    ): CatalogResult {
        conn.send(PacketCodec.gameListRequest(knownRevision))
        when (val packet = conn.receive()) {
            is IncomingPacket.Catalog -> {
                val games = packet.value.games.sortedWith(
                    compareBy({ it.systemName.lowercase() }, { it.displayName.lowercase() }),
                )
                return CatalogResult(host, controlPort, games, packet.value.catalogRevision)
            }
            is IncomingPacket.Error -> error(packet.value.message)
            else -> error("expected GameList, got $packet")
        }
    }
}

data class JoinedPlaySession(
    val host: String,
    val controlPort: Int,
    val inputPort: Int,
    val welcome: HostWelcome,
    val seats: SeatAssignment,
    val ready: SessionReady,
    val starting: SessionStarting?,
    val media: MediaEndpoint?,
    @Volatile var videoPlayer: RtpVideoPlayer?,
    val audioPlayer: RtpOpusPlayer?,
    private val control: ControlConnection,
    private val inputSocket: DatagramSocket,
    private val inputAddress: InetAddress,
) : AutoCloseable {
    private var inputSequence: Long = 0
    private var keyboardSequence: Long = 0
    private var heartbeatSequence: Long = 0
    private val pendingSoftKeyboard = AtomicReference<SoftKeyboardRequest?>(null)

    @Volatile
    private var stagingPlayer: RtpVideoPlayer? = null

    @Volatile
    private var stagingUri: String? = null

    @Volatile
    private var stagingReadySent = false

    /** Headless decode probe so cutover ACK waits for real frames, not just RTP AUs. */
    private var stagingProbeReader: ImageReader? = null
    private var stagingProbeThread: HandlerThread? = null

    @Volatile
    var wantedTier: Int = MediaQualityTier.Medium.id

    @Volatile
    var wantedSize: Int = MediaStreamSize.P540.id

    @Volatile
    var displayLayout: Int = DisplayLayoutPreference.Landscape.id

    fun sendController(localPlayer: Int, state: ControllerState) {
        sendControllerCopies(localPlayer, state, copies = 1)
    }

    /**
     * UDP pad sample(s). On edges, [copies] > 1 with distinct timestamps so the host
     * still sees the press if one datagram is lost (mirrors desktop kChangeCopies).
     */
    fun sendControllerCopies(localPlayer: Int, state: ControllerState, copies: Int) {
        val n = copies.coerceAtLeast(1)
        val baseTs = System.nanoTime() / 1_000L
        for (copy in 0 until n) {
            inputSequence += 1
            val stamped = state.copy(
                sequence = inputSequence,
                timestampUs = baseTs + copy,
            )
            val packet = PacketCodec.controllerInput(
                clientId = welcome.clientId,
                localPlayer = localPlayer,
                state = stamped,
            )
            val datagram = DatagramPacket(packet, packet.size, inputAddress, inputPort)
            inputSocket.send(datagram)
        }
    }

    fun sendKeyboard(keys: Int, localPlayer: Int = 0) {
        keyboardSequence += 1
        val packet = PacketCodec.keyboardInput(
            clientId = welcome.clientId,
            localPlayer = localPlayer,
            sequence = keyboardSequence,
            timestampUs = System.nanoTime() / 1_000L,
            keys = keys,
        )
        val datagram = DatagramPacket(packet, packet.size, inputAddress, inputPort)
        inputSocket.send(datagram)
    }

    /** Normalized DS bottom-screen touch (0..65535); host converts to stylus pixels. */
    fun sendTouch(x: Int, y: Int, pressed: Boolean, localPlayer: Int = 0) {
        inputSequence += 1
        val packet = PacketCodec.touchInput(
            clientId = welcome.clientId,
            localPlayer = localPlayer,
            sequence = inputSequence,
            timestampUs = System.nanoTime() / 1_000L,
            x = x,
            y = y,
            pressed = pressed,
        )
        val datagram = DatagramPacket(packet, packet.size, inputAddress, inputPort)
        inputSocket.send(datagram)
    }

    /** Press then release a remoted key (e.g. P → PAUSE_TOGGLE). */
    fun pulseKeyboardKey(keyMask: Int, holdMs: Long = 40L) {
        sendKeyboard(keyMask)
        Thread.sleep(holdMs)
        sendKeyboard(0)
    }

    fun sendHeartbeat() {
        val stats = videoPlayer?.takeHeartbeatStats()
        val frames = stats?.framesDecodedDelta ?: 0
        val loss = stats?.lossPermille ?: 0
        control.send(
            PacketCodec.viewerHeartbeat(
                clientId = welcome.clientId,
                sequence = heartbeatSequence++,
                lossPermille = loss.coerceIn(0, 0xffff),
                framesDecodedDelta = frames.coerceIn(0, 0xffff),
                wantedTier = wantedTier,
                wantedSize = wantedSize,
                displayLayout = displayLayout,
            ),
        )
    }

    fun sendSoftKeyboardResponse(response: SoftKeyboardResponse) {
        control.send(
            PacketCodec.softKeyboardResponse(
                requestId = response.requestId,
                accepted = response.accepted,
                text = response.text,
            ),
        )
    }

    fun sendEmulatorControl(
        pause: EmulatorControlState = EmulatorControlState.Unchanged,
        fastForward: EmulatorControlState = EmulatorControlState.Unchanged,
    ) {
        control.send(
            PacketCodec.emulatorControl(
                clientId = welcome.clientId,
                pause = pause,
                fastForward = fastForward,
            ),
        )
    }

    fun sendDiscControl(
        gameId: String,
        action: DiscControlAction,
        discIndex: Int = 0,
    ) {
        control.send(
            PacketCodec.discControlRequest(
                gameId = gameId,
                action = action,
                discIndex = discIndex,
            ),
        )
    }

    fun sendLinkRequest(
        gameId: String,
        targetUsername: String,
        action: LinkAction,
    ) {
        control.send(
            PacketCodec.linkRequest(
                gameId = gameId,
                targetUsername = targetUsername,
                action = action,
            ),
        )
    }

    /** Non-blocking poll of the TCP control channel (SoftKeyboard / SessionEnded / …). */
    fun tryReceiveControl(timeoutMs: Int = 500): IncomingPacket? = control.tryReceive(timeoutMs)

    fun offerSoftKeyboard(request: SoftKeyboardRequest) {
        pendingSoftKeyboard.set(request)
    }

    fun takePendingSoftKeyboard(): SoftKeyboardRequest? = pendingSoftKeyboard.getAndSet(null)

    /**
     * Host is warming a new encode on [videoUri]. Bind a staging receiver; once it
     * decodes frames we ACK with MediaVideoReady so the host can drop the old path.
     * Returns false if bind failed (and NACKs the host with an empty MediaVideoReady).
     */
    fun beginVideoPending(videoUri: String): Boolean {
        if (videoUri.isBlank()) return false
        if (stagingUri == videoUri && stagingPlayer != null) return true
        clearStagingProbe()
        runCatching { stagingPlayer?.close() }
        stagingPlayer = null
        stagingUri = null
        stagingReadySent = false
        return try {
            val port = MediaUris.portFrom(videoUri, MediaUris.H264_SCHEME)
            if (port == videoPlayer?.port) {
                control.send(PacketCodec.mediaVideoReady(videoUri))
                stagingReadySent = true
                true
            } else {
                val player = RtpVideoPlayer(port).also { it.startReceiving() }
                // Decode without a visible view so Ready means "frames out", not "UDP heard".
                val thread = HandlerThread("staging-probe-$port").also { it.start() }
                val reader = ImageReader.newInstance(1920, 1080, ImageFormat.PRIVATE, 3)
                reader.setOnImageAvailableListener(
                    { r -> runCatching { r.acquireLatestImage()?.close() } },
                    Handler(thread.looper),
                )
                player.attachSurface(reader.surface)
                stagingProbeThread = thread
                stagingProbeReader = reader
                stagingPlayer = player
                stagingUri = videoUri
                true
            }
        } catch (t: Throwable) {
            clearStagingProbe()
            stagingPlayer = null
            stagingUri = null
            // Empty URI → host aborts in-flight cutover instead of waiting out the timeout.
            runCatching { control.send(PacketCodec.mediaVideoReady("")) }
            ClientFileLog.append(
                "Video staging bind failed for $videoUri: ${t.javaClass.simpleName}: ${t.message}",
            )
            false
        }
    }

    /**
     * If staging has decoded frames, send MediaVideoReady once and return the URI.
     * Caller promotes [videoPlayer] after the host confirms via MediaEndpoint.
     */
    fun pollVideoCutoverReady(): String? {
        val uri = stagingUri ?: return null
        val staging = stagingPlayer ?: return null
        if (stagingReadySent) return null
        if (!staging.hasDecodedFrames()) return null
        return try {
            control.send(PacketCodec.mediaVideoReady(uri))
            stagingReadySent = true
            uri
        } catch (_: Throwable) {
            null
        }
    }

    /**
     * After host cutover completes it resends MediaEndpoint — adopt staging as the
     * live player (or rebind). Returns the player that should drive the UI.
     */
    fun promoteVideo(endpoint: MediaEndpoint): RtpVideoPlayer? {
        val uri = endpoint.videoUri
        if (uri.isBlank()) return videoPlayer
        val port = runCatching {
            MediaUris.portFrom(uri, MediaUris.H264_SCHEME)
        }.getOrNull() ?: return videoPlayer

        val staging = stagingPlayer
        if (staging != null && staging.port == port) {
            val previous = videoPlayer
            // Drop the headless probe surface; the UI will attach a real Surface.
            staging.detachSurface()
            clearStagingProbe()
            videoPlayer = staging
            stagingPlayer = null
            stagingUri = null
            stagingReadySent = false
            runCatching { previous?.close() }
            return videoPlayer
        }

        if (videoPlayer?.port == port) {
            clearStagingProbe()
            runCatching { stagingPlayer?.close() }
            stagingPlayer = null
            stagingUri = null
            stagingReadySent = false
            return videoPlayer
        }

        clearStagingProbe()
        runCatching { stagingPlayer?.close() }
        stagingPlayer = null
        stagingUri = null
        stagingReadySent = false
        runCatching { videoPlayer?.close() }
        videoPlayer = RtpVideoPlayer(port).also { it.startReceiving() }
        return videoPlayer
    }

    /**
     * Explicit Leave button path. Must run off the main thread (network I/O).
     * Returns true if ClientSessionLeave was written to the socket.
     */
    fun leave(reason: String = "client left"): Boolean {
        if (!control.isConnected()) return false
        control.send(PacketCodec.clientSessionLeave(welcome.clientId, reason))
        return true
    }

    /** Send ClientSessionLeave (best-effort) then tear down sockets/media. */
    override fun close() {
        runCatching { leave() }
        closeAfterLeave()
    }

    /** Tear down without leave — use after leave was already sent, or when the link is dead. */
    fun closeAfterLeave() {
        clearStagingProbe()
        runCatching { stagingPlayer?.close() }
        stagingPlayer = null
        stagingUri = null
        runCatching { videoPlayer?.close() }
        runCatching { audioPlayer?.close() }
        runCatching { control.close() }
        runCatching { inputSocket.close() }
    }

    private fun clearStagingProbe() {
        runCatching { stagingProbeReader?.close() }
        stagingProbeReader = null
        stagingProbeThread?.quitSafely()
        stagingProbeThread = null
    }
}

/**
 * Host control accept always expects GameListRequest first on a connection, then
 * ClientHello on that same socket (see host_session_helpers / session_lobby).
 */
object SessionJoiner {
    fun join(
        host: String,
        controlPort: Int,
        inputPort: Int,
        username: String,
        password: String,
        game: GameInfo,
        receiveAudio: Boolean = true,
        displayLayout: Int = DisplayLayoutPreference.Landscape.id,
        controllerName: String = "Android Touch Pad",
        controllerGuid: String = "android-touch-0",
        onPasswordChangeRequired: (() -> String)? = null,
    ): JoinedPlaySession {
        val control = ControlConnection(host, controlPort)
        var videoPlayer: RtpVideoPlayer? = null
        var audioPlayer: RtpOpusPlayer? = null
        try {
            control.connect()

            control.send(PacketCodec.gameListRequest(revision = 0L))
            when (val catalog = control.receive()) {
                is IncomingPacket.Catalog -> { /* ok */ }
                is IncomingPacket.Error -> error(catalog.value.message)
                else -> error("expected GameList before hello, got $catalog")
            }

            val controllers = listOf(
                ControllerInfo(
                    localPlayer = 0,
                    name = controllerName,
                    guid = controllerGuid,
                ),
            )
            var sessionPassword = password
            control.send(
                PacketCodec.clientHello(
                    username = username,
                    displayName = username,
                    selectedGameId = game.id,
                    sessionMode = GameSessionMode.SinglePlayer,
                    requestedPlayers = 1,
                    controllers = controllers,
                    wantsVideo = true,
                    wantsAudio = receiveAudio,
                    displayLayout = displayLayout,
                    password = sessionPassword,
                ),
            )

            // Fresh start and seat-reconnect use the same packet order on the host.
            // PasswordChangeRequired may arrive before HostWelcome for default-password users.
            var welcome: HostWelcome? = null
            while (welcome == null) {
                when (val p = control.receive()) {
                    is IncomingPacket.Welcome -> welcome = p.value
                    is IncomingPacket.PasswordChangeRequired -> {
                        val changer = onPasswordChangeRequired
                            ?: error("host requires a password change")
                        val newPassword = changer().also {
                            require(it.isNotEmpty()) { "password change cancelled" }
                        }
                        control.send(
                            PacketCodec.passwordChange(
                                username = username,
                                currentPassword = sessionPassword,
                                newPassword = newPassword,
                            ),
                        )
                        sessionPassword = newPassword
                    }
                    is IncomingPacket.Error -> error(p.value.message)
                    else -> error("expected HostWelcome, got $p")
                }
            }
            val seats = when (val p = control.receive()) {
                is IncomingPacket.Seats -> p.value
                is IncomingPacket.Error -> error(p.value.message)
                else -> error("expected SeatAssignment, got $p")
            }
            val ready = when (val p = control.receive()) {
                is IncomingPacket.Ready -> p.value
                is IncomingPacket.Error -> error(p.value.message)
                else -> error("expected SessionReady, got $p")
            }

            var media: MediaEndpoint? = null
            var starting: SessionStarting? = null

            // Bind media UDP as soon as MediaEndpoint arrives so we catch the first packets.
            // SoftKeyboardRequest can arrive early; stash it for the play loop.
            var earlySoftKeyboard: SoftKeyboardRequest? = null
            while (starting == null) {
                when (val p = control.receive()) {
                    is IncomingPacket.Media -> {
                        media = p.value
                        if (videoPlayer == null && p.value.videoUri.isNotBlank()) {
                            val port = MediaUris.portFrom(p.value.videoUri, MediaUris.H264_SCHEME)
                            videoPlayer = RtpVideoPlayer(port).also { it.startReceiving() }
                        }
                        if (receiveAudio && audioPlayer == null && p.value.audioUri.isNotBlank()) {
                            val port = MediaUris.portFrom(p.value.audioUri, MediaUris.OPUS_SCHEME)
                            audioPlayer = RtpOpusPlayer(port).also { it.start() }
                        }
                    }
                    is IncomingPacket.Starting -> starting = p.value
                    is IncomingPacket.SoftKeyboard -> earlySoftKeyboard = p.value
                    is IncomingPacket.Error -> error(p.value.message)
                    is IncomingPacket.Ended -> error("session ended early: ${p.value.reason}")
                    else -> { /* ignore unknown */ }
                }
            }

            val inputSocket = DatagramSocket()
            return JoinedPlaySession(
                host = host,
                controlPort = controlPort,
                inputPort = inputPort,
                welcome = welcome,
                seats = seats,
                ready = ready,
                starting = starting,
                media = media,
                videoPlayer = videoPlayer,
                audioPlayer = audioPlayer,
                control = control,
                inputSocket = inputSocket,
                inputAddress = InetAddress.getByName(host),
            ).also { joined ->
                joined.displayLayout = displayLayout
                earlySoftKeyboard?.let { joined.offerSoftKeyboard(it) }
                // Ownership transferred — do not close in finally.
                videoPlayer = null
                audioPlayer = null
            }
        } catch (t: Throwable) {
            runCatching { videoPlayer?.close() }
            runCatching { audioPlayer?.close() }
            runCatching { control.close() }
            throw t
        }
    }
}
