package com.archstreamer.client.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.archstreamer.client.net.CatalogFetcher
import com.archstreamer.client.net.JoinedPlaySession
import com.archstreamer.client.net.SessionJoiner
import com.archstreamer.client.protocol.ControllerState
import com.archstreamer.client.protocol.GameInfo
import com.archstreamer.client.protocol.Protocol
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

sealed interface Screen {
    data object Connect : Screen
    data object Catalog : Screen
    data object Playing : Screen
}

data class UiState(
    val screen: Screen = Screen.Connect,
    val host: String = "",
    val controlPort: String = Protocol.DEFAULT_CONTROL_PORT.toString(),
    val inputPort: String = Protocol.DEFAULT_INPUT_PORT.toString(),
    val username: String = "android",
    val busy: Boolean = false,
    val status: String = "Enter host LAN IP and connect.",
    val games: List<GameInfo> = emptyList(),
    val filter: String = "",
    val selectedGame: GameInfo? = null,
    val mediaHint: String = "",
)

class ClientViewModel : ViewModel() {
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private var session: JoinedPlaySession? = null
    private var inputJob: Job? = null
    private var latestPad = ControllerState()

    fun onHostChange(value: String) = _state.update { it.copy(host = value.trim()) }
    fun onControlPortChange(value: String) = _state.update { it.copy(controlPort = value.trim()) }
    fun onInputPortChange(value: String) = _state.update { it.copy(inputPort = value.trim()) }
    fun onUsernameChange(value: String) = _state.update { it.copy(username = value.trim()) }
    fun onFilterChange(value: String) = _state.update { it.copy(filter = value) }

    fun connect() {
        val snap = _state.value
        val host = snap.host
        val port = snap.controlPort.toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT
        if (host.isBlank()) {
            _state.update { it.copy(status = "Host IP is required.") }
            return
        }

        viewModelScope.launch {
            _state.update { it.copy(busy = true, status = "Fetching catalog from $host:$port…") }
            runCatching {
                withContext(Dispatchers.IO) {
                    CatalogFetcher.fetch(host, port)
                }
            }.onSuccess { catalog ->
                _state.update {
                    it.copy(
                        busy = false,
                        screen = Screen.Catalog,
                        games = catalog.games,
                        status = "Loaded ${catalog.games.size} games (rev ${catalog.catalogRevision}).",
                    )
                }
            }.onFailure { err ->
                _state.update {
                    it.copy(
                        busy = false,
                        status = "Connect failed: ${err.message ?: err}",
                    )
                }
            }
        }
    }

    fun disconnectToConnect() {
        endSession()
        _state.update {
            it.copy(
                screen = Screen.Connect,
                games = emptyList(),
                selectedGame = null,
                mediaHint = "",
                status = "Disconnected.",
            )
        }
    }

    fun startGame(game: GameInfo) {
        val snap = _state.value
        val host = snap.host
        val controlPort = snap.controlPort.toIntOrNull() ?: Protocol.DEFAULT_CONTROL_PORT
        val inputPort = snap.inputPort.toIntOrNull() ?: Protocol.DEFAULT_INPUT_PORT
        val username = snap.username.ifBlank { "android" }

        viewModelScope.launch {
            _state.update {
                it.copy(
                    busy = true,
                    status = "Starting ${game.displayName}…",
                    selectedGame = game,
                )
            }
            runCatching {
                withContext(Dispatchers.IO) {
                    endSessionLocked()
                    SessionJoiner.join(host, controlPort, inputPort, username, game)
                }
            }.onSuccess { joined ->
                session = joined
                startInputLoop()
                val media = joined.media
                _state.update {
                    it.copy(
                        busy = false,
                        screen = Screen.Playing,
                        status = "Playing ${game.displayName} (client ${joined.welcome.clientId}).",
                        mediaHint = if (media != null) {
                            "Video URI from host:\n${media.videoUri}\n\n" +
                                "Decode not implemented yet — pad input is live over UDP."
                        } else {
                            "No MediaEndpoint yet — pad input still sends over UDP."
                        },
                    )
                }
            }.onFailure { err ->
                _state.update {
                    it.copy(
                        busy = false,
                        screen = Screen.Catalog,
                        status = "Start failed: ${err.message ?: err}",
                    )
                }
            }
        }
    }

    fun onPadState(state: ControllerState) {
        latestPad = state
    }

    fun leavePlay() {
        endSession()
        _state.update {
            it.copy(
                screen = Screen.Catalog,
                selectedGame = null,
                mediaHint = "",
                status = "Left session.",
            )
        }
    }

    private fun startInputLoop() {
        inputJob?.cancel()
        inputJob = viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                session?.sendController(localPlayer = 0, state = latestPad)
                delay(4L) // ~250 Hz, same idea as the desktop client
            }
        }
    }

    private fun endSession() {
        inputJob?.cancel()
        inputJob = null
        session?.close()
        session = null
        latestPad = ControllerState()
    }

    private fun endSessionLocked() {
        inputJob?.cancel()
        inputJob = null
        session?.close()
        session = null
    }

    override fun onCleared() {
        endSession()
        super.onCleared()
    }
}
