package com.archstreamer.client.protocol

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets

/** Little-endian reader/writer matching src/common/serialization.cpp */
class WireReader(private val bytes: ByteArray, private var offset: Int = 0) {
    fun remaining(): Int = bytes.size - offset

    fun readU8(): Int {
        require(1)
        return bytes[offset++].toInt() and 0xff
    }

    fun readU16(): Int {
        require(2)
        val value = (bytes[offset].toInt() and 0xff) or
            ((bytes[offset + 1].toInt() and 0xff) shl 8)
        offset += 2
        return value
    }

    fun readU32(): Long {
        require(4)
        var value = 0L
        for (i in 0 until 4) {
            value = value or ((bytes[offset + i].toLong() and 0xff) shl (i * 8))
        }
        offset += 4
        return value
    }

    fun readI16(): Short {
        return readU16().toShort()
    }

    fun readU64(): Long {
        require(8)
        var value = 0L
        for (i in 0 until 8) {
            value = value or ((bytes[offset + i].toLong() and 0xff) shl (i * 8))
        }
        offset += 8
        return value
    }

    fun readBool(): Boolean = readU8() != 0

    fun readString(): String {
        val size = readU16()
        require(size)
        val text = String(bytes, offset, size, StandardCharsets.UTF_8)
        offset += size
        return text
    }

    fun readBytes(): ByteArray {
        val size = readU32().toInt()
        require(size >= 0)
        if (size > 16 * 1024 * 1024) error("binary payload too large ($size)")
        require(size)
        val value = bytes.copyOfRange(offset, offset + size)
        offset += size
        return value
    }

    fun readOptionalString(): String? {
        if (!readBool()) return null
        return readString()
    }

    private fun require(count: Int) {
        if (offset + count > bytes.size) {
            error("truncated protocol packet (need $count at offset $offset, size ${bytes.size})")
        }
    }
}

class WireWriter {
    private val out = ArrayList<Byte>(256)

    fun writeU8(value: Int) {
        out.add((value and 0xff).toByte())
    }

    fun writeU16(value: Int) {
        out.add((value and 0xff).toByte())
        out.add(((value ushr 8) and 0xff).toByte())
    }

    fun writeU32(value: Long) {
        var v = value
        repeat(4) {
            out.add((v and 0xff).toByte())
            v = v ushr 8
        }
    }

    fun writeI16(value: Short) {
        writeU16(value.toInt() and 0xffff)
    }

    fun writeU64(value: Long) {
        var v = value
        repeat(8) {
            out.add((v and 0xff).toByte())
            v = v ushr 8
        }
    }

    fun writeBool(value: Boolean) {
        writeU8(if (value) 1 else 0)
    }

    fun writeString(value: String) {
        val utf8 = value.toByteArray(StandardCharsets.UTF_8)
        check(utf8.size <= 0xffff) { "string too large for protocol packet" }
        writeU16(utf8.size)
        utf8.forEach { out.add(it) }
    }

    fun writeOptionalString(value: String?) {
        writeBool(value != null)
        if (value != null) writeString(value)
    }

    fun writeBytes(value: ByteArray) {
        check(value.size <= 16 * 1024 * 1024) { "binary payload too large for protocol packet" }
        writeU32(value.size.toLong())
        value.forEach { out.add(it) }
    }

    fun toByteArray(): ByteArray = out.toByteArray()
}

object PacketCodec {
    fun wrap(type: PacketType, payload: ByteArray): ByteArray {
        val writer = WireWriter()
        writer.writeU32(Protocol.MAGIC.toLong() and 0xffffffffL)
        writer.writeU16(Protocol.VERSION)
        writer.writeU8(type.id)
        writer.writeU32(payload.size.toLong())
        return writer.toByteArray() + payload
    }

    fun parseHeader(bytes: ByteArray): Triple<Int, PacketType, Int> {
        val reader = WireReader(bytes)
        val magic = reader.readU32().toInt()
        check(magic == Protocol.MAGIC) {
            "bad magic 0x${magic.toString(16)} (expected ARST)"
        }
        val version = reader.readU16()
        check(version == Protocol.VERSION) {
            "unsupported protocol version $version (need ${Protocol.VERSION})"
        }
        val type = PacketType.fromId(reader.readU8())
        val payloadSize = reader.readU32().toInt()
        return Triple(version, type, payloadSize)
    }

    fun gameListRequest(revision: Long = 0L): ByteArray {
        val payload = WireWriter().apply { writeU64(revision) }.toByteArray()
        return wrap(PacketType.GameListRequest, payload)
    }

    fun clientHello(
        username: String,
        displayName: String,
        selectedGameId: String?,
        sessionMode: GameSessionMode,
        requestedPlayers: Int,
        controllers: List<ControllerInfo>,
        wantsVideo: Boolean = true,
        wantsAudio: Boolean = true,
        displayLayout: Int = DisplayLayoutPreference.Auto.id,
        password: String = "",
        clientBlocksRevision: Long = 0L,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeString(username)
            writeString(displayName)
            writeOptionalString(selectedGameId)
            writeU8(sessionMode.id)
            writeU8(requestedPlayers)
            writeU8(controllers.size)
            for (c in controllers) {
                writeU8(c.localPlayer)
                writeString(c.name)
                writeString(c.guid)
                writeU16(c.vendorId)
                writeU16(c.productId)
            }
            writeBool(wantsVideo)
            writeBool(wantsAudio)
            writeU8(displayLayout)
            writeString(password)
            writeU64(clientBlocksRevision)
        }.toByteArray()
        return wrap(PacketType.ClientHello, payload)
    }

    fun controllerInput(
        clientId: Int,
        localPlayer: Int,
        state: ControllerState,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeU8(clientId)
            writeU8(localPlayer)
            writeU32(state.sequence)
            writeU64(state.timestampUs)
            writeU32(state.buttons.toLong() and 0xffffffffL)
            writeI16(state.leftX)
            writeI16(state.leftY)
            writeI16(state.rightX)
            writeI16(state.rightY)
            writeU16(state.leftTrigger)
            writeU16(state.rightTrigger)
        }.toByteArray()
        return wrap(PacketType.ControllerInput, payload)
    }

    fun clientSessionLeave(clientId: Int, reason: String): ByteArray {
        val payload = WireWriter().apply {
            writeU8(clientId)
            writeString(reason)
        }.toByteArray()
        return wrap(PacketType.ClientSessionLeave, payload)
    }

    fun softKeyboardResponse(
        requestId: Long,
        accepted: Boolean,
        text: String,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeU32(requestId)
            writeBool(accepted)
            writeString(text)
        }.toByteArray()
        return wrap(PacketType.SoftKeyboardResponse, payload)
    }

    fun mediaVideoReady(videoUri: String): ByteArray {
        val payload = WireWriter().apply {
            writeString(videoUri)
        }.toByteArray()
        return wrap(PacketType.MediaVideoReady, payload)
    }

    fun emulatorControl(
        clientId: Int,
        pause: EmulatorControlState = EmulatorControlState.Unchanged,
        fastForward: EmulatorControlState = EmulatorControlState.Unchanged,
        force: Boolean = false,
        action: Int = EmulatorControlAction.None,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeU8(clientId)
            writeU8(pause.id)
            writeU8(fastForward.id)
            writeU8(if (force) 1 else 0)
            writeU8(action)
        }.toByteArray()
        return wrap(PacketType.EmulatorControl, payload)
    }

    fun activeSessionInfoRequest(): ByteArray =
        wrap(PacketType.ActiveSessionInfoRequest, ByteArray(0))

    fun discControlRequest(
        gameId: String,
        action: DiscControlAction,
        discIndex: Int = 0,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeString(gameId)
            writeU8(action.id)
            writeU8(discIndex.coerceIn(0, 255))
        }.toByteArray()
        return wrap(PacketType.DiscControlRequest, payload)
    }

    fun linkRequest(
        gameId: String,
        targetUsername: String,
        action: LinkAction,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeString(gameId)
            writeString(targetUsername)
            writeU8(action.id)
        }.toByteArray()
        return wrap(PacketType.LinkRequest, payload)
    }

    fun clientLogBundle(
        username: String,
        sessionCount: Int,
        textUtf8: ByteArray,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeString(username)
            writeU32(sessionCount.toLong() and 0xffffffffL)
            writeBytes(textUtf8)
        }.toByteArray()
        return wrap(PacketType.ClientLogBundle, payload)
    }

    fun passwordChange(
        username: String,
        currentPassword: String,
        newPassword: String,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeString(username)
            writeString(currentPassword)
            writeString(newPassword)
        }.toByteArray()
        return wrap(PacketType.PasswordChange, payload)
    }

    fun lobbyPresence(
        username: String,
        password: String,
        clientBlocksRevision: Long = 0L,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeString(username)
            writeString(password)
            writeU64(clientBlocksRevision)
        }.toByteArray()
        return wrap(PacketType.LobbyPresence, payload)
    }

    fun controlsDbPull(username: String): ByteArray {
        val payload = WireWriter().apply {
            writeString(username)
        }.toByteArray()
        return wrap(PacketType.ControlsDbPull, payload)
    }

    fun controlsDbPush(username: String, dbBytes: ByteArray): ByteArray {
        val payload = WireWriter().apply {
            writeString(username)
            writeBytes(dbBytes)
        }.toByteArray()
        return wrap(PacketType.ControlsDbPush, payload)
    }

    fun artAssetRequest(assetKey: String, role: String, cachedSha256: String = ""): ByteArray {
        val payload = WireWriter().apply {
            writeString(assetKey)
            writeString(role)
            writeString(cachedSha256)
        }.toByteArray()
        return wrap(PacketType.ArtAssetRequest, payload)
    }

    fun keyboardInput(
        clientId: Int,
        localPlayer: Int,
        sequence: Long,
        timestampUs: Long,
        keys: Int,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeU8(clientId)
            writeU8(localPlayer)
            writeU32(sequence)
            writeU64(timestampUs)
            writeU32(keys.toLong() and 0xffffffffL)
        }.toByteArray()
        return wrap(PacketType.KeyboardInput, payload)
    }

    fun touchInput(
        clientId: Int,
        localPlayer: Int,
        sequence: Long,
        timestampUs: Long,
        x: Int,
        y: Int,
        pressed: Boolean,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeU8(clientId)
            writeU8(localPlayer)
            writeU32(sequence)
            writeU64(timestampUs)
            writeU16(x.coerceIn(0, 65535))
            writeU16(y.coerceIn(0, 65535))
            writeBool(pressed)
        }.toByteArray()
        return wrap(PacketType.TouchInput, payload)
    }

    fun viewerHeartbeat(
        clientId: Int,
        sequence: Long,
        lossPermille: Int = 0,
        framesDecodedDelta: Int = 0,
        wantedTier: Int = MediaQualityTier.Medium.id,
        maxBitrateKbps: Int = 0,
        showFramecount: Boolean = false,
        wantedSize: Int = MediaStreamSize.P540.id,
        displayLayout: Int = DisplayLayoutPreference.Auto.id,
        wantedFeel: Int = MediaStreamFeel.LowLatency.id,
        wantedBitrate: Int = MediaStreamBitrate.Kbps3500.id,
    ): ByteArray {
        val payload = WireWriter().apply {
            writeU8(clientId)
            writeU32(sequence)
            writeU16(lossPermille)
            writeU16(framesDecodedDelta)
            writeU8(wantedTier)
            writeU16(maxBitrateKbps)
            writeBool(showFramecount)
            writeU8(wantedSize)
            writeU8(displayLayout)
            writeU8(wantedFeel)
            writeU8(wantedBitrate)
        }.toByteArray()
        return wrap(PacketType.ViewerHeartbeat, payload)
    }

    fun decode(type: PacketType, payload: ByteArray): IncomingPacket {
        val reader = WireReader(payload)
        return when (type) {
            PacketType.GameList -> IncomingPacket.Catalog(readGameList(reader))
            PacketType.HostWelcome -> IncomingPacket.Welcome(
                HostWelcome(
                    clientId = reader.readU8(),
                    maxPlayersForClient = reader.readU8(),
                    hostIsPlayer = reader.readBool(),
                ),
            )
            PacketType.SeatAssignment -> {
                val count = reader.readU8()
                val seats = List(count) {
                    PlayerSeat(
                        clientId = reader.readU8(),
                        localPlayer = reader.readU8(),
                        retroarchPort = reader.readU8(),
                    )
                }
                IncomingPacket.Seats(SeatAssignment(seats))
            }
            PacketType.SessionReady -> IncomingPacket.Ready(
                SessionReady(
                    selectedGameId = reader.readString(),
                    sessionMode = modeFromId(reader.readU8()),
                    playerCount = reader.readU8(),
                ),
            )
            PacketType.SessionStarting -> IncomingPacket.Starting(
                SessionStarting(
                    selectedGameId = reader.readString(),
                    sessionMode = modeFromId(reader.readU8()),
                    playerCount = reader.readU8(),
                ),
            )
            PacketType.SessionEnded -> IncomingPacket.Ended(SessionEnded(reader.readString()))
            PacketType.MediaEndpoint -> IncomingPacket.Media(
                MediaEndpoint(
                    videoUri = reader.readString(),
                    audioUri = reader.readString(),
                ),
            )
            PacketType.MediaVideoPending -> IncomingPacket.VideoPending(reader.readString())
            PacketType.SoftKeyboardRequest -> IncomingPacket.SoftKeyboard(
                SoftKeyboardRequest(
                    requestId = reader.readU32(),
                    prompt = reader.readString(),
                    initialText = reader.readString(),
                    maxLength = reader.readU8().let { if (it == 0) 12 else it },
                ),
            )
            PacketType.DsScreenLayout -> {
                val windowW = reader.readU16()
                val windowH = reader.readU16()
                val flags = reader.readU8()
                IncomingPacket.DsScreens(
                    DsScreenLayout(
                        windowW = windowW,
                        windowH = windowH,
                        hasTop = flags and 0x01 != 0,
                        topX = reader.readI16().toInt(),
                        topY = reader.readI16().toInt(),
                        topW = reader.readI16().toInt(),
                        topH = reader.readI16().toInt(),
                        hasBot = flags and 0x02 != 0,
                        botX = reader.readI16().toInt(),
                        botY = reader.readI16().toInt(),
                        botW = reader.readI16().toInt(),
                        botH = reader.readI16().toInt(),
                    ),
                )
            }
            PacketType.DiscControlResponse -> IncomingPacket.DiscControl(
                DiscControlResponse(
                    ok = reader.readBool(),
                    discIndex = reader.readU8(),
                    discCount = reader.readU8(),
                    message = reader.readString(),
                ),
            )
            PacketType.LinkResponse -> IncomingPacket.Link(
                LinkResponse(
                    ok = reader.readBool(),
                    status = LinkStatus.fromId(reader.readU8()),
                    peerUsername = reader.readString(),
                    message = reader.readString(),
                ),
            )
            PacketType.ArtAssetResponse -> {
                val assetKey = reader.readString()
                val role = reader.readString()
                val found = reader.readBool()
                val extension = reader.readString()
                val data = reader.readBytes()
                val contentSha256 = if (reader.remaining() > 0) reader.readString() else ""
                IncomingPacket.Art(
                    ArtAssetResponse(
                        assetKey = assetKey,
                        role = role,
                        found = found,
                        extension = extension,
                        data = data,
                        contentSha256 = contentSha256,
                    ),
                )
            }
            PacketType.Error -> IncomingPacket.Error(ErrorPacket(reader.readString()))
            PacketType.ActiveSessionInfo -> {
                val active = reader.readBool()
                val selectedGameId = reader.readOptionalString()
                val sessionMode = modeFromId(reader.readU8())
                val playerCount = reader.readU8()
                val connectedPlayers = reader.readU8()
                val disconnectedPlayers = reader.readU8()
                val viewerCount = reader.readU8()
                val videoEnabled = reader.readBool()
                val audioEnabled = reader.readBool()
                val activeSlots = if (reader.remaining() >= 2) reader.readU8() else null
                val maxSlots = if (activeSlots != null) reader.readU8() else null
                IncomingPacket.ActiveSession(
                    ActiveSessionInfo(
                        active = active,
                        selectedGameId = selectedGameId,
                        sessionMode = sessionMode,
                        playerCount = playerCount,
                        connectedPlayers = connectedPlayers,
                        disconnectedPlayers = disconnectedPlayers,
                        viewerCount = viewerCount,
                        videoEnabled = videoEnabled,
                        audioEnabled = audioEnabled,
                        activeSlots = activeSlots,
                        maxSlots = maxSlots,
                    ),
                )
            }
            PacketType.PasswordChangeRequired -> IncomingPacket.PasswordChangeRequired
            PacketType.LobbyPresenceAck -> IncomingPacket.LobbyPresenceAck(reader.readU8())
            PacketType.ControlsDbResponse -> IncomingPacket.ControlsDb(
                username = reader.readString(),
                found = reader.readBool(),
                dbBytes = reader.readBytes(),
            )
            PacketType.ControlsDbAck -> IncomingPacket.ControlsDbAck(
                username = reader.readString(),
                ok = reader.readBool(),
                message = reader.readString(),
            )
            PacketType.CatalogUserBlocks -> IncomingPacket.CatalogBlocks(
                blocksRevision = reader.readU64(),
                full = reader.readBool(),
                blockedGameIds = List(reader.readU16()) { reader.readString() },
            )
            else -> IncomingPacket.Unknown(type)
        }
    }

    private fun modeFromId(id: Int): GameSessionMode =
        GameSessionMode.entries.firstOrNull { it.id == id } ?: GameSessionMode.SinglePlayer

    private fun readGameList(reader: WireReader): GameList {
        val revision = reader.readU64()
        val full = reader.readBool()
        val gameCount = reader.readU16()
        val games = List(gameCount) {
            GameInfo(
                id = reader.readString(),
                identityKey = reader.readString(),
                assetKey = reader.readString(),
                displayName = reader.readString(),
                systemName = reader.readString(),
                systemKey = reader.readString(),
                coreName = reader.readString(),
                canonicalName = reader.readString(),
                version = reader.readString(),
                language = reader.readString(),
                region = reader.readString(),
                supportsSingleplayer = reader.readBool(),
                supportsMultiplayer = reader.readBool(),
                minPlayers = reader.readU8(),
                maxPlayers = reader.readU8(),
                updatedAt = reader.readU64(),
                playlistDiscs = List(reader.readU16()) { reader.readString() },
            )
        }
        val deleted = List(reader.readU16()) { reader.readString() }
        return GameList(revision, full, games, deleted)
    }
}

/** Convenience for unit-style checks without Android deps. */
fun byteBufferLe(capacity: Int): ByteBuffer =
    ByteBuffer.allocate(capacity).order(ByteOrder.LITTLE_ENDIAN)
