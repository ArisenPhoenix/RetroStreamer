package com.archstreamer.client.ui

import android.content.pm.ActivityInfo
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.activity.ComponentActivity
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.viewinterop.AndroidView
import com.archstreamer.client.media.RtpVideoPlayer

/**
 * TextureView (not SurfaceView) so Compose pad overlays stay on top and receive touches.
 * Letterboxes to the stream aspect ratio.
 *
 * When [hybridPortraitStack] is true and the stream is still wide (Hybrid), crops the
 * left/right Hybrid panes into a stacked portrait presentation that fills the phone.
 */
@Composable
fun StreamVideoView(
    player: RtpVideoPlayer?,
    modifier: Modifier = Modifier,
    aspectRatio: Float = 16f / 9f,
    hybridPortraitStack: Boolean = false,
) {
    var reportedAspect by remember(player) { mutableFloatStateOf(aspectRatio) }
    val ratio = if (reportedAspect > 0.1f) reportedAspect else aspectRatio
    // Hybrid is wide (~16:9); Top/Bottom is tall (~2:3). Only rearrange Hybrid.
    val useHybridStack = hybridPortraitStack && ratio > 1.15f

    DisposableEffect(player) {
        player?.onVideoSize = { w, h ->
            if (w > 0 && h > 0) {
                reportedAspect = w.toFloat() / h.toFloat()
            }
        }
        onDispose {
            player?.onVideoSize = null
        }
    }

    if (useHybridStack) {
        AndroidView(
            modifier = modifier
                .fillMaxSize()
                .background(Color.Black),
            factory = { context ->
                DsHybridPortraitView(context).also { view ->
                    view.streamAspect = ratio
                    view.onSurfaceReady = { surface ->
                        (view.tag as? RtpVideoPlayer)?.attachSurface(surface)
                    }
                    view.onSurfaceDestroyed = {
                        (view.tag as? RtpVideoPlayer)?.detachSurface()
                    }
                }
            },
            update = { view ->
                view.streamAspect = ratio
                val previous = view.tag as? RtpVideoPlayer
                if (previous !== player) {
                    view.tag = player
                    // Surface attach is driven by onSurfaceReady; rebind only on player swap.
                    view.currentDecoderSurface()?.let { surface ->
                        player?.attachSurface(surface)
                    }
                }
            },
        )
        return
    }

    BoxWithConstraints(
        modifier = modifier
            .fillMaxSize()
            .background(Color.Black),
        contentAlignment = Alignment.Center,
    ) {
        val maxW = maxWidth
        val maxH = maxHeight
        val boxW = if (maxW / maxH > ratio) maxH * ratio else maxW
        val boxH = if (maxW / maxH > ratio) maxH else maxW / ratio

        AndroidView(
            modifier = Modifier.size(boxW, boxH),
            factory = { context ->
                PlayerTextureView(context)
            },
            update = { view ->
                view.bindPlayer(player)
            },
        )
    }
}

/**
 * Owns a single [Surface] for the TextureView and only re-attaches when the
 * [RtpVideoPlayer] identity changes — Compose `update` must not recreate Surfaces.
 */
private class PlayerTextureView(context: android.content.Context) : TextureView(context) {
    private var boundPlayer: RtpVideoPlayer? = null
    private var ownedSurface: Surface? = null

    init {
        surfaceTextureListener = object : SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(
                surface: SurfaceTexture,
                width: Int,
                height: Int,
            ) {
                replaceOwnedSurface(Surface(surface))
                boundPlayer?.attachSurface(ownedSurface!!)
            }

            override fun onSurfaceTextureSizeChanged(
                surface: SurfaceTexture,
                width: Int,
                height: Int,
            ) = Unit

            override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
                boundPlayer?.detachSurface()
                releaseOwnedSurface()
                return true
            }

            override fun onSurfaceTextureUpdated(surface: SurfaceTexture) = Unit
        }
    }

    fun bindPlayer(player: RtpVideoPlayer?) {
        if (boundPlayer === player) return
        boundPlayer = player
        val surface = ownedSurface
        if (player != null && surface != null && isAvailable) {
            player.attachSurface(surface)
        }
    }

    private fun replaceOwnedSurface(surface: Surface) {
        releaseOwnedSurface()
        ownedSurface = surface
    }

    private fun releaseOwnedSurface() {
        ownedSurface?.release()
        ownedSurface = null
    }
}

@Composable
fun LockLandscapeWhileVisible() {
    val activity = LocalContext.current as? ComponentActivity
    DisposableEffect(activity) {
        val previous = activity?.requestedOrientation
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        onDispose {
            activity?.requestedOrientation =
                previous ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }
}

/** Prevents the display from sleeping while the play surface is shown. */
@Composable
fun KeepScreenOnWhileVisible() {
    val view = LocalView.current
    DisposableEffect(view) {
        val previous = view.keepScreenOn
        view.keepScreenOn = true
        onDispose { view.keepScreenOn = previous }
    }
}

/** DualScreen (DS): allow portrait for Top/Bottom and landscape for Hybrid. */
@Composable
fun UnlockSensorOrientationWhileVisible() {
    val activity = LocalContext.current as? ComponentActivity
    DisposableEffect(activity) {
        val previous = activity?.requestedOrientation
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR
        onDispose {
            activity?.requestedOrientation =
                previous ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
    }
}
