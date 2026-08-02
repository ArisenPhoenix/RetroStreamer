package com.archstreamer.client.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Green = Color(0xFF2E7D4F)
private val Ink = Color(0xFF121816)

private val DarkColors = darkColorScheme(
    primary = Green,
    background = Ink,
    surface = Color(0xFF1A2220),
)

private val LightColors = lightColorScheme(
    primary = Green,
    background = Color(0xFFF3F6F4),
    surface = Color(0xFFFFFFFF),
)

@Composable
fun ArchStreamerTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColors else LightColors,
        content = content,
    )
}
