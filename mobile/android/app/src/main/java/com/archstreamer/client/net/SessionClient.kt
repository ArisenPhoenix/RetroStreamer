package com.archstreamer.client.net

import com.archstreamer.client.protocol.ControllerInfo
import com.archstreamer.client.protocol.ControllerState
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.GameSessionMode
import com.archstreamer.client.protocol.HostWelcome
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.MediaEndpoint
import com.archstreamer.client.protocol.PacketCodec
import com.archstreamer.client.protocol.Protocol
import com.archstreamer.client.protocol.SeatAssignment
import com.archstreamer.client.protocol.SessionReady
import com.archstreamer.client.protocol.SessionStarting
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

data class CatalogResult(
    val host: String,
    val controlPort: Int,
    val games: List<GameInfo>,
    val catalogRevision: Long,
)

/**
 * Matches ClientSessionService::begin — open TCP, ask for GameList, return catalog.
 * Connection is closed afterward; joining a game uses a fresh session.
 */
object CatalogFetcher {
    fun fetch(
        host: String,
        controlPort: Int = Protocol.DEFAULT_CONTROL_PORT,
        knownRevision: Long = 0L,
    ): CatalogResult {
        ControlConnection(host, controlPort).use { conn ->
            conn.connect()
            conn.send(PacketCodec.gameListRequest(knownRevision))
            when (val packet = conn.receive()) {
                is IncomingPacket.Catalog -> {
                    // MVP: always treat as full list (revision 0 request).
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
    val control: ControlConnection,
    val inputSocket: DatagramSocket,
    val inputAddress: InetAddress,
) : AutoCloseable {
    private var inputSequence: Long = 0

    fun sendController(localPlayer: Int, state: ControllerState) {
        inputSequence += 1
        val stamped = state.copy(
            sequence = inputSequence,
            timestampUs = System.nanoTime() / 1_000L,
        )
        val packet = PacketCodec.controllerInput(
            clientId = welcome.clientId,
            localPlayer = localPlayer,
            state = stamped,
        )
        val datagram = DatagramPacket(packet, packet.size, inputAddress, inputPort)
        inputSocket.send(datagram)
    }

    fun leave(reason: String = "client left") {
        runCatching {
            control.send(
                PacketCodec.clientSessionLeave(welcome.clientId, reason),
            )
        }
    }

    override fun close() {
        leave()
        runCatching { control.close() }
        runCatching { inputSocket.close() }
    }
}

/**
 * Matches finish_join + wait_for_starting (partial): hello → welcome → seats → ready,
 * then drain MediaEndpoint / SessionStarting. Video decode is still a later step.
 */
object SessionJoiner {
    fun join(
        host: String,
        controlPort: Int,
        inputPort: Int,
        username: String,
        game: GameInfo,
    ): JoinedPlaySession {
        val control = ControlConnection(host, controlPort)
        control.connect()

        val controllers = listOf(
            ControllerInfo(
                localPlayer = 0,
                name = "Android Touch Pad",
                guid = "android-touch-0",
            ),
        )
        control.send(
            PacketCodec.clientHello(
                username = username,
                displayName = username,
                selectedGameId = game.id,
                sessionMode = GameSessionMode.SinglePlayer,
                requestedPlayers = 1,
                controllers = controllers,
            ),
        )

        val welcome = when (val p = control.receive()) {
            is IncomingPacket.Welcome -> p.value
            is IncomingPacket.Error -> error(p.value.message)
            else -> error("expected HostWelcome, got $p")
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
        // Host may send MediaEndpoint before SessionStarting (same as wait_for_starting).
        while (starting == null) {
            when (val p = control.receive()) {
                is IncomingPacket.Media -> media = p.value
                is IncomingPacket.Starting -> starting = p.value
                is IncomingPacket.Error -> error(p.value.message)
                is IncomingPacket.Ended -> error("session ended early: ${p.value.reason}")
                else -> { /* ignore unknown for forward-compat */ }
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
            control = control,
            inputSocket = inputSocket,
            inputAddress = InetAddress.getByName(host),
        )
    }
}
