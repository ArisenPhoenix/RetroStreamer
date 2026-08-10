package com.archstreamer.client

import android.app.ActivityManager
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import com.archstreamer.client.protocol.ClientDeviceCapabilities
import com.archstreamer.client.protocol.ClientDeviceClass
import com.archstreamer.client.protocol.ClientPerformanceClass

data class AndroidDeviceProfile(
    val deviceClass: ClientDeviceClass,
    val performanceClass: ClientPerformanceClass,
    val hardwareThreads: Int,
    val memoryClassMb: Int,
    val screenWidth: Int,
    val screenHeight: Int,
    val platformVersion: Int,
) {
    val isTv: Boolean = deviceClass == ClientDeviceClass.Tv

    fun toCapabilities(): ClientDeviceCapabilities =
        ClientDeviceCapabilities(
            deviceClass = deviceClass,
            performanceClass = performanceClass,
            hardwareThreads = hardwareThreads,
            screenWidth = screenWidth,
            screenHeight = screenHeight,
            platformVersion = platformVersion,
        )

    companion object {
        fun from(context: Context): AndroidDeviceProfile {
            val app = context.applicationContext
            val config = app.resources.configuration
            val metrics = app.resources.displayMetrics
            val deviceClass = deviceClassFor(config)
            val threads = Runtime.getRuntime().availableProcessors().coerceIn(0, 255)
            val memoryClass = (app.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager)
                ?.memoryClass
                ?: 0
            return AndroidDeviceProfile(
                deviceClass = deviceClass,
                performanceClass = performanceClassFor(deviceClass, threads, memoryClass),
                hardwareThreads = threads,
                memoryClassMb = memoryClass,
                screenWidth = metrics.widthPixels,
                screenHeight = metrics.heightPixels,
                platformVersion = Build.VERSION.SDK_INT,
            )
        }

        fun isTv(configuration: Configuration): Boolean =
            deviceClassFor(configuration) == ClientDeviceClass.Tv

        fun deviceClassFor(configuration: Configuration): ClientDeviceClass {
            val uiMode = configuration.uiMode and Configuration.UI_MODE_TYPE_MASK
            return when (uiMode) {
                Configuration.UI_MODE_TYPE_TELEVISION -> ClientDeviceClass.Tv
                Configuration.UI_MODE_TYPE_WATCH -> ClientDeviceClass.Handheld
                else -> if (configuration.smallestScreenWidthDp >= 600) {
                    ClientDeviceClass.Tablet
                } else {
                    ClientDeviceClass.Phone
                }
            }
        }

        private fun performanceClassFor(
            deviceClass: ClientDeviceClass,
            threads: Int,
            memoryClassMb: Int,
        ): ClientPerformanceClass =
            when {
                threads >= 8 && memoryClassMb >= 384 -> ClientPerformanceClass.High
                threads >= 6 && memoryClassMb >= 192 -> ClientPerformanceClass.Medium
                threads >= 4 && memoryClassMb >= 128 && deviceClass != ClientDeviceClass.Tv ->
                    ClientPerformanceClass.Medium
                else -> ClientPerformanceClass.Low
            }
    }
}

data class ClientInputPolicy(
    val forcePhysicalControllerWhenConnected: Boolean,
    val requireControllerForPlay: Boolean,
    val useTvPlayMenu: Boolean,
    val closeMenuRailWhenEnteringGames: Boolean,
)

fun clientInputPolicyFor(deviceClass: ClientDeviceClass): ClientInputPolicy =
    if (deviceClass == ClientDeviceClass.Tv) {
        ClientInputPolicy(
            forcePhysicalControllerWhenConnected = true,
            requireControllerForPlay = true,
            useTvPlayMenu = true,
            closeMenuRailWhenEnteringGames = true,
        )
    } else {
        ClientInputPolicy(
            forcePhysicalControllerWhenConnected = false,
            requireControllerForPlay = false,
            useTvPlayMenu = false,
            closeMenuRailWhenEnteringGames = false,
        )
    }
