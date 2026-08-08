package com.archstreamer.client

import android.Manifest
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import com.archstreamer.client.net.ClientFileLog
import com.archstreamer.client.ui.ArchStreamerApp
import com.archstreamer.client.ui.ClientViewModel
import com.archstreamer.client.ui.WithoutSoftKeyboard
import com.archstreamer.client.ui.theme.ArchStreamerTheme

class MainActivity : ComponentActivity() {
    private val viewModel: ClientViewModel by viewModels()

    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { /* optional */ }

    private val bluetoothPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) {
            viewModel.refreshPhysicalPads()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    // TV remote Back: open menu → focus hamburger → exit (not D-pad Left).
                    if (!viewModel.handleSystemBack()) {
                        finish()
                    }
                }
            },
        )
        setContent {
            ArchStreamerTheme {
                val state by viewModel.state.collectAsState()
                LaunchedEffect(state.playing) {
                    if (state.playing) {
                        ensureNotificationPermission()
                    }
                }
                LaunchedEffect(state.controls.usePhysicalController) {
                    if (state.controls.usePhysicalController) {
                        ensureBluetoothPermission()
                    }
                }
                Box(modifier = Modifier.fillMaxSize()) {
                    // Compose alone does not always receive keyboard attach/detach
                    // config callbacks; a View sink keeps Activity from missing them
                    // (see detachable-keyboard guidance). Manifest lists keyboard|… .
                    AndroidView(
                        factory = { ctx ->
                            object : View(ctx) {
                                override fun onConfigurationChanged(newConfig: Configuration) {
                                    super.onConfigurationChanged(newConfig)
                                }
                            }
                        },
                        modifier = Modifier.size(0.dp),
                    )
                    Surface(
                        modifier = Modifier.fillMaxSize(),
                        color = MaterialTheme.colorScheme.background,
                    ) {
                        // One gate for every field in the app, dialogs included: with a
                        // keyboard attached the IME is never asked for.
                        WithoutSoftKeyboard(state.controls.hasKeyboardActive) {
                            ArchStreamerApp(viewModel = viewModel)
                        }
                    }
                }
            }
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        // BT keyboard wake often flips keyboard / keyboardHidden / navigation without
        // recreating us (configChanges). Refresh pads; log when connections debug is on.
        ClientFileLog.conn(
            "configChanged keyboard=${newConfig.keyboard} " +
                "keyboardHidden=${newConfig.keyboardHidden} " +
                "navigation=${newConfig.navigation}",
        )
        viewModel.refreshPhysicalPads()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        viewModel.noteKeyboardUse(event)
        // Real pads must hit the gamepad tracker before play-keyboard remotes, otherwise
        // DPAD_* keys are stolen as "keyboard D-pad" and BUTTON_* never update latestPad
        // when hasKeys filtering rejected the device.
        val fromPad = viewModel.isPhysicalGamepadDevice(event.deviceId)
        if (fromPad && viewModel.onGamepadKeyEvent(event)) return true
        if (viewModel.onPlayKeyEvent(event)) return true
        if (!fromPad && viewModel.onGamepadKeyEvent(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (viewModel.onGamepadMotionEvent(event)) return true
        return super.dispatchGenericMotionEvent(event)
    }

    private fun ensureNotificationPermission() {
        if (Build.VERSION.SDK_INT < 33) return
        val granted = ContextCompat.checkSelfPermission(
            this,
            Manifest.permission.POST_NOTIFICATIONS,
        ) == PackageManager.PERMISSION_GRANTED
        if (!granted) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private fun ensureBluetoothPermission() {
        if (Build.VERSION.SDK_INT < 31) return
        val granted = ContextCompat.checkSelfPermission(
            this,
            Manifest.permission.BLUETOOTH_CONNECT,
        ) == PackageManager.PERMISSION_GRANTED
        if (!granted) {
            bluetoothPermission.launch(Manifest.permission.BLUETOOTH_CONNECT)
        }
    }
}
