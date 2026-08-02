package com.archstreamer.client.protocol

/**
 * Wire types matching include/common/protocol.hpp (ProtocolVersion 16).
 * Keep field order identical to the C++ serializers.
 */
object Protocol {
    const val MAGIC: Int = 0x41525354 // "ARST"
    const val VERSION: Int = 16
    const val HEADER_SIZE: Int = 11 // u32 + u16 + u8 + u32, little-endian, no padding

    const val DEFAULT_CONTROL_PORT: Int = 45555
    const val DEFAULT_INPUT_PORT: Int = 45454
}

enum class PacketType(val id: Int) {
    ClientHello(1),
    HostWelcome(2),
    ClientConfig(3),
    SeatAssignment(4),
    ControllerInput(5),
    ViewerHeartbeat(6),
    Error(7),
    GameListRequest(8),
    GameList(9),
    SessionReady(10),
    SessionStarting(11),
    SessionEnded(12),
    MediaEndpoint(13),
    ActiveSessionInfoRequest(14),
    ActiveSessionInfo(15),
    ArtAssetRequest(16),
    ArtAssetResponse(17),
    DiscControlRequest(18),
    DiscControlResponse(19),
    KeyboardInput(20),
    ClientSessionLeave(21),
    LinkRequest(22),
    LinkResponse(23),
    SoftKeyboardRequest(24),
    SoftKeyboardResponse(25),
    MediaVideoPending(26),
    MediaVideoReady(27);

    companion object {
        fun fromId(id: Int): PacketType =
            entries.firstOrNull { it.id == id }
                ?: error("unknown packet type $id")
    }
}

enum class GameSessionMode(val id: Int) {
    SinglePlayer(0),
    Multiplayer(1),
}

data class GameInfo(
    val id: String,
    val identityKey: String,
    val assetKey: String,
    val displayName: String,
    val systemName: String,
    val systemKey: String,
    val coreName: String,
    val canonicalName: String,
    val version: String,
    val language: String,
    val region: String,
    val supportsSingleplayer: Boolean,
    val supportsMultiplayer: Boolean,
    val minPlayers: Int,
    val maxPlayers: Int,
    val updatedAt: Long,
    val playlistDiscs: List<String>,
)

data class GameList(
    val catalogRevision: Long,
    val full: Boolean,
    val games: List<GameInfo>,
    val deletedGameIds: List<String>,
)

data class HostWelcome(
    val clientId: Int,
    val maxPlayersForClient: Int,
    val hostIsPlayer: Boolean,
)

data class PlayerSeat(
    val clientId: Int,
    val localPlayer: Int,
    val retroarchPort: Int,
)

data class SeatAssignment(
    val seats: List<PlayerSeat>,
)

data class SessionReady(
    val selectedGameId: String,
    val sessionMode: GameSessionMode,
    val playerCount: Int,
)

data class SessionStarting(
    val selectedGameId: String,
    val sessionMode: GameSessionMode,
    val playerCount: Int,
)

data class SessionEnded(
    val reason: String,
)

data class MediaEndpoint(
    val videoUri: String,
    val audioUri: String,
)

data class ErrorPacket(
    val message: String,
)

data class ControllerInfo(
    val localPlayer: Int,
    val name: String,
    val guid: String,
    val vendorId: Int = 0,
    val productId: Int = 0,
)

/** Matches struct ControllerState / ControllerButton in controller_state.hpp */
data class ControllerState(
    val sequence: Long = 0,
    val timestampUs: Long = 0,
    val buttons: Int = 0,
    val leftX: Short = 0,
    val leftY: Short = 0,
    val rightX: Short = 0,
    val rightY: Short = 0,
    val leftTrigger: Int = 0,
    val rightTrigger: Int = 0,
) {
    companion object {
        const val BUTTON_A = 1 shl 0
        const val BUTTON_B = 1 shl 1
        const val BUTTON_X = 1 shl 2
        const val BUTTON_Y = 1 shl 3
        const val BUTTON_BACK = 1 shl 4
        const val BUTTON_GUIDE = 1 shl 5
        const val BUTTON_START = 1 shl 6
        const val BUTTON_LEFT_STICK = 1 shl 7
        const val BUTTON_RIGHT_STICK = 1 shl 8
        const val BUTTON_LEFT_SHOULDER = 1 shl 9
        const val BUTTON_RIGHT_SHOULDER = 1 shl 10
        const val BUTTON_DPAD_UP = 1 shl 11
        const val BUTTON_DPAD_DOWN = 1 shl 12
        const val BUTTON_DPAD_LEFT = 1 shl 13
        const val BUTTON_DPAD_RIGHT = 1 shl 14
    }
}

sealed class IncomingPacket {
    data class Welcome(val value: HostWelcome) : IncomingPacket()
    data class Seats(val value: SeatAssignment) : IncomingPacket()
    data class Ready(val value: SessionReady) : IncomingPacket()
    data class Starting(val value: SessionStarting) : IncomingPacket()
    data class Ended(val value: SessionEnded) : IncomingPacket()
    data class Media(val value: MediaEndpoint) : IncomingPacket()
    data class Catalog(val value: GameList) : IncomingPacket()
    data class Error(val value: ErrorPacket) : IncomingPacket()
    data class Unknown(val type: PacketType) : IncomingPacket()
}
