package com.archstreamer.client.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.archstreamer.client.protocol.GameInfo

@Composable
fun ArchStreamerApp(viewModel: ClientViewModel) {
    val state by viewModel.state.collectAsState()
    when (state.screen) {
        Screen.Connect -> ConnectScreen(state, viewModel)
        Screen.Catalog -> CatalogScreen(state, viewModel)
        Screen.Playing -> PlayScreen(state, viewModel)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ConnectScreen(state: UiState, viewModel: ClientViewModel) {
    Scaffold(
        topBar = { TopAppBar(title = { Text("ArchStreamer") }) },
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .padding(16.dp)
                .fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                "LAN client — same Wi‑Fi as the host.",
                style = MaterialTheme.typography.bodyMedium,
            )
            OutlinedTextField(
                value = state.host,
                onValueChange = viewModel::onHostChange,
                label = { Text("Host IP") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("192.168.x.x") },
            )
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
            OutlinedTextField(
                value = state.username,
                onValueChange = viewModel::onUsernameChange,
                label = { Text("Username (save profile)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = viewModel::connect,
                enabled = !state.busy,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(if (state.busy) "Connecting…" else "Connect")
            }
            if (state.busy) {
                CircularProgressIndicator(modifier = Modifier.align(Alignment.CenterHorizontally))
            }
            Text(state.status, style = MaterialTheme.typography.bodySmall)
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CatalogScreen(state: UiState, viewModel: ClientViewModel) {
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

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Games") },
                navigationIcon = {
                    TextButton(onClick = viewModel::disconnectToConnect) { Text("Back") }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize(),
        ) {
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
            if (state.busy) {
                Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
            } else {
                LazyColumn(
                    contentPadding = PaddingValues(vertical = 8.dp),
                    modifier = Modifier.fillMaxSize(),
                ) {
                    items(games, key = { it.id }) { game ->
                        GameRow(game) { viewModel.startGame(game) }
                    }
                }
            }
        }
    }
}

@Composable
private fun GameRow(game: GameInfo, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(game.displayName) },
        supportingContent = { Text(game.systemName.ifBlank { game.systemKey }) },
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PlayScreen(state: UiState, viewModel: ClientViewModel) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(state.selectedGame?.displayName ?: "Playing") },
                actions = {
                    TextButton(onClick = viewModel::leavePlay) { Text("Leave") }
                },
            )
        },
    ) { padding ->
        Box(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize(),
        ) {
            Column(
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(16.dp),
            ) {
                Text(state.status)
                Text(
                    state.mediaHint,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.padding(top = 8.dp),
                )
            }
            GamepadOverlay(
                modifier = Modifier.fillMaxSize(),
                onState = viewModel::onPadState,
            )
        }
    }
}
