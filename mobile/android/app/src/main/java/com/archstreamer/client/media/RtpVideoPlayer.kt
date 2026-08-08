package com.archstreamer.client.media

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import com.archstreamer.client.BuildConfig
import com.archstreamer.client.net.ClientFileLog
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.min

/**
 * Binds the host-assigned RTP video UDP port, depayloads H.264, and decodes to a Surface.
 * Call [startReceiving] as soon as MediaEndpoint arrives (before the Surface exists);
 * NALs queue until [attachSurface] configures MediaCodec.
 */
class RtpVideoPlayer(
    private val listenPort: Int,
) : AutoCloseable {
    private val running = AtomicBoolean(false)
    private val framesDecoded = AtomicInteger(0)
    private val framesDecodedDelta = AtomicInteger(0)
    private val pipelineDead = AtomicBoolean(false)
    private val lastErrorMs = AtomicLong(0)

    private val nalQueue = ArrayBlockingQueue<ByteArray>(12)
    private val depay = RtpH264Depayloader()
    private val reorderBuffer = RtpReorderBuffer(REORDER_BUFFER_PACKETS)
    private val accessUnitsReceived = AtomicInteger(0)

    @Volatile private var surface: Surface? = null
    @Volatile private var codec: MediaCodec? = null
    @Volatile private var codecConfigured = false
    private val codecConfiguring = AtomicBoolean(false)
    @Volatile private var sps: ByteArray? = null
    @Volatile private var pps: ByteArray? = null
    /** Surface identity currently configured into MediaCodec (avoid tear-down churn). */
    @Volatile private var attachedSurfaceIdentity: Int? = null

    /** Optional UI hook when the decoder reports a real frame size. */
    @Volatile var onVideoSize: ((width: Int, height: Int) -> Unit)? = null

    private var receiveThread: Thread? = null
    private var decodeThread: HandlerThread? = null
    private var decodeHandler: Handler? = null
    private var socket: DatagramSocket? = null

    // TEMP: frame pacing debug — remove when judder investigation is done.
    // Gate: debug APK + Settings → Debug → Log connections.
    private val paceAu = FramePaceWindow("au", listenPort)
    private val pacePresent = FramePaceWindow("present", listenPort)

    val port: Int get() = listenPort

    fun takeFramesDecodedDelta(): Int = framesDecodedDelta.getAndSet(0)

    /**
     * Heartbeat counters for the host Auto ladder.
     * [lossPermille] is RTP loss over the interval, or 1000 if the receive/decode
     * pipeline is dead (mirrors desktop gst receiver death).
     */
    fun takeHeartbeatStats(): HeartbeatStats {
        val frames = framesDecodedDelta.getAndSet(0)
        val packets = depay.takePacketStats()
        val total = packets.received + packets.lost
        val rtpLoss = if (total > 0) {
            ((packets.lost * 1000L) / total).toInt().coerceIn(0, 1000)
        } else {
            0
        }
        val dead = pipelineDead.get() || !running.get()
        val loss = if (dead) {
            1000
        } else {
            rtpLoss
        }
        if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
            paceAu.flushIfDue()?.let { ClientFileLog.conn(it) }
            pacePresent.flushIfDue()?.let { ClientFileLog.conn(it) }
        }
        return HeartbeatStats(
            framesDecodedDelta = frames,
            lossPermille = loss,
            packetsReceived = packets.received,
            packetsLost = packets.lost,
            sequenceGaps = packets.sequenceGaps,
            pipelineDead = dead,
        )
    }

    /** True once RTP has delivered at least one H.264 access unit (no Surface needed). */
    fun hasReceivedAccessUnits(): Boolean = accessUnitsReceived.get() > 0 || sps != null

    /** True once MediaCodec has released at least one decoded frame to the Surface. */
    fun hasDecodedFrames(): Boolean = framesDecoded.get() > 0

    fun startReceiving() {
        if (!running.compareAndSet(false, true)) return
        pipelineDead.set(false)
        socket = DatagramSocket(null).apply {
            reuseAddress = true
            receiveBufferSize = 4 * 1024 * 1024
            bind(InetSocketAddress(listenPort))
            soTimeout = 1000
        }
        receiveThread = Thread({
            val buf = ByteArray(2048)
            val packet = DatagramPacket(buf, buf.size)
            while (running.get()) {
                try {
                    socket?.receive(packet) ?: break
                    reorderBuffer.push(packet.data, packet.length) { data, length ->
                        val au = depay.push(data, length)
                        if (depay.consumeResyncRequested()) {
                            nalQueue.clear()
                        }
                        if (au != null) {
                            offerNal(au)
                            maybeConfigureFromSpsPps(au)
                            scheduleDecode()
                        }
                    }
                } catch (_: java.net.SocketTimeoutException) {
                    // keep waiting
                } catch (t: Throwable) {
                    if (running.get()) {
                        Log.w(TAG, "RTP receive error on :$listenPort", t)
                        lastErrorMs.set(System.currentTimeMillis())
                        pipelineDead.set(true)
                    }
                }
            }
        }, "rtp-h264-$listenPort").also { it.isDaemon = true; it.start() }

        decodeThread = HandlerThread("h264-decode-$listenPort").also { it.start() }
        decodeHandler = Handler(decodeThread!!.looper)
        Log.i(TAG, "Listening for RTP H.264 on UDP $listenPort")
    }

    fun attachSurface(newSurface: Surface) {
        val identity = System.identityHashCode(newSurface)
        if (surface === newSurface && attachedSurfaceIdentity == identity && codecConfigured) {
            return
        }
        val previous = surface
        surface = newSurface
        attachedSurfaceIdentity = identity

        // Prefer a live output-surface swap so orientation / view resize does not
        // wait on a new IDR (full codec rebuild).
        val existing = codec
        if (existing != null && codecConfigured && previous != null && previous !== newSurface) {
            try {
                existing.setOutputSurface(newSurface)
                scheduleDecode()
                return
            } catch (t: Throwable) {
                Log.w(TAG, "setOutputSurface failed; reconfiguring codec", t)
            }
        }

        codecConfigured = false
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        codec = null
        // Reconfigure once SPS/PPS known (or already queued).
        sps?.let { s -> pps?.let { p -> configureCodec(s, p) } }
        scheduleDecode()
    }

    fun detachSurface() {
        surface = null
        attachedSurfaceIdentity = null
        codecConfigured = false
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        codec = null
    }

    override fun close() {
        running.set(false)
        runCatching { socket?.close() }
        socket = null
        receiveThread?.join(1500)
        receiveThread = null
        detachSurface()
        decodeThread?.quitSafely()
        decodeThread = null
        decodeHandler = null
        nalQueue.clear()
    }

    private fun offerNal(au: ByteArray) {
        accessUnitsReceived.incrementAndGet()
        if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
            paceAu.record()
        }
        if (!nalQueue.offer(au)) {
            nalQueue.poll()
            nalQueue.offer(au)
        }
    }

    private fun maybeConfigureFromSpsPps(au: ByteArray) {
        if (codecConfigured) return
        var i = 0
        while (i + 4 < au.size) {
            if (au[i] == 0.toByte() && au[i + 1] == 0.toByte() &&
                au[i + 2] == 0.toByte() && au[i + 3] == 1.toByte()
            ) {
                val nalStart = i + 4
                if (nalStart >= au.size) break
                val type = au[nalStart].toInt() and 0x1f
                val next = nextStartCode(au, nalStart) ?: au.size
                val nal = au.copyOfRange(nalStart, next)
                when (type) {
                    7 -> sps = nal
                    8 -> pps = nal
                }
                i = next
            } else {
                i++
            }
        }
        val s = sps
        val p = pps
        if (s != null && p != null && surface != null && !codecConfigured) {
            val lastError = lastErrorMs.get()
            if (lastError != 0L && System.currentTimeMillis() - lastError < CODEC_RETRY_DELAY_MS) {
                return
            }
            configureCodec(s, p)
        }
    }

    private fun nextStartCode(data: ByteArray, from: Int): Int? {
        var i = from
        while (i + 3 < data.size) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte()
            ) {
                return i
            }
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte() && data[i + 2] == 1.toByte()) {
                return i
            }
            i++
        }
        return null
    }

    private fun configureCodec(spsNal: ByteArray, ppsNal: ByteArray) {
        val surf = surface ?: return
        if (!codecConfiguring.compareAndSet(false, true)) return
        var candidate: MediaCodec? = null
        try {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            codec = null
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1920, 1080)
            format.setByteBuffer("csd-0", java.nio.ByteBuffer.wrap(startCodePrefixed(spsNal)))
            format.setByteBuffer("csd-1", java.nio.ByteBuffer.wrap(startCodePrefixed(ppsNal)))
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            val c = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            candidate = c
            c.configure(format, surf, null, 0)
            c.start()
            codec = c
            candidate = null
            codecConfigured = true
            pipelineDead.set(false)
            lastErrorMs.set(0)
            Log.i(TAG, "MediaCodec configured for port $listenPort")
        } catch (t: Throwable) {
            Log.e(TAG, "MediaCodec configure failed", t)
            codecConfigured = false
            runCatching { candidate?.stop() }
            runCatching { candidate?.release() }
            codec = null
            pipelineDead.set(true)
            lastErrorMs.set(System.currentTimeMillis())
        } finally {
            codecConfiguring.set(false)
        }
    }

    private fun startCodePrefixed(nal: ByteArray): ByteArray {
        if (nal.size >= 4 && nal[0] == 0.toByte() && nal[1] == 0.toByte() &&
            nal[2] == 0.toByte() && nal[3] == 1.toByte()
        ) {
            return nal
        }
        return byteArrayOf(0, 0, 0, 1) + nal
    }

    private fun scheduleDecode() {
        decodeHandler?.post { drainDecode() }
    }

    private fun drainDecode() {
        val c = codec
        if (c == null || !codecConfigured) return
        while (true) {
            val au = nalQueue.poll() ?: break
            try {
                val inIndex = c.dequeueInputBuffer(2_000)
                if (inIndex < 0) {
                    // Put back and try later.
                    nalQueue.offer(au)
                    decodeHandler?.postDelayed({ drainDecode() }, 2)
                    return
                }
                val input = c.getInputBuffer(inIndex) ?: continue
                input.clear()
                input.put(au)
                c.queueInputBuffer(inIndex, 0, au.size, System.nanoTime() / 1000, 0)

                val info = MediaCodec.BufferInfo()
                var outIndex = c.dequeueOutputBuffer(info, 0)
                while (outIndex != MediaCodec.INFO_TRY_AGAIN_LATER) {
                    when {
                        outIndex >= 0 -> {
                            c.releaseOutputBuffer(outIndex, true)
                            framesDecoded.incrementAndGet()
                            framesDecodedDelta.incrementAndGet()
                            pipelineDead.set(false)
                            if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
                                pacePresent.record()
                            }
                        }
                        outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val fmt = c.outputFormat
                            val w = fmt.getInteger(MediaFormat.KEY_WIDTH)
                            val h = fmt.getInteger(MediaFormat.KEY_HEIGHT)
                            if (w > 0 && h > 0) {
                                onVideoSize?.invoke(w, h)
                            }
                        }
                    }
                    outIndex = c.dequeueOutputBuffer(info, 0)
                }
            } catch (t: Throwable) {
                Log.w(TAG, "decode error", t)
                codecConfigured = false
                pipelineDead.set(true)
                lastErrorMs.set(System.currentTimeMillis())
                nalQueue.clear()
                depay.resetUntilIdr()
                runCatching { c.stop() }
                runCatching { c.release() }
                codec = null
                return
            }
        }
    }

    data class HeartbeatStats(
        val framesDecodedDelta: Int,
        val lossPermille: Int,
        val packetsReceived: Long = 0,
        val packetsLost: Long = 0,
        val sequenceGaps: Long = 0,
        val pipelineDead: Boolean = false,
    )

    /**
     * TEMP: 1 Hz Δt summary for frame-pacing debug.
     * Delete this class + paceAu/pacePresent call sites when done.
     */
    private class FramePaceWindow(
        private val label: String,
        private val port: Int,
    ) {
        private val lock = Any()
        private var lastNs = 0L
        private var windowStartNs = 0L
        private val dtsMs = ArrayList<Double>(64)

        fun record() {
            val now = System.nanoTime()
            synchronized(lock) {
                if (lastNs != 0L) {
                    val dt = (now - lastNs) / 1_000_000.0
                    if (dt > 0.0 && dt < 1000.0) {
                        dtsMs.add(dt)
                    }
                }
                lastNs = now
                if (windowStartNs == 0L) {
                    windowStartNs = now
                }
            }
        }

        fun flushIfDue(): String? {
            val now = System.nanoTime()
            synchronized(lock) {
                if (windowStartNs == 0L || now - windowStartNs < 1_000_000_000L) {
                    return null
                }
                if (dtsMs.isEmpty()) {
                    windowStartNs = now
                    return null
                }
                dtsMs.sort()
                val n = dtsMs.size
                val p50 = dtsMs[n / 2]
                val p95 = dtsMs[min(n - 1, (n * 95) / 100)]
                val maxDt = dtsMs.last()
                val avg = dtsMs.sum() / n
                val line =
                    "pace $label port=$port n=$n dt_ms " +
                        "avg=${"%.1f".format(avg)} p50=${"%.1f".format(p50)} " +
                        "p95=${"%.1f".format(p95)} max=${"%.1f".format(maxDt)}"
                dtsMs.clear()
                windowStartNs = now
                return line
            }
        }
    }

    /**
     * Tiny RTP reorder buffer. In-order packets pass through immediately; small forward
     * jumps wait briefly for the missing packet before a real loss is declared downstream.
     */
    private class RtpReorderBuffer(
        private val maxPending: Int,
    ) {
        private val pending = HashMap<Int, PendingPacket>()
        private var expectedSeq: Int? = null

        fun push(src: ByteArray, length: Int, emit: (ByteArray, Int) -> Unit) {
            if (length < RTP_HEADER_MIN_BYTES) return
            val seq = sequence(src)
            val expected = expectedSeq
            if (expected == null) {
                expectedSeq = next(seq)
                emit(src, length)
                drain(emit)
                return
            }

            val ahead = sequenceDistance(seq, expected)
            when {
                ahead == 0 -> {
                    expectedSeq = next(seq)
                    emit(src, length)
                    drain(emit)
                }
                ahead > 0 -> {
                    pending[seq] = PendingPacket(src.copyOf(length), length)
                    if (pending.size >= maxPending) {
                        skipMissingUntilReady(emit)
                    }
                }
                else -> Unit // late duplicate; the decoder has already moved past it.
            }
        }

        private fun drain(emit: (ByteArray, Int) -> Unit) {
            while (true) {
                val expected = expectedSeq ?: return
                val packet = pending.remove(expected) ?: return
                expectedSeq = next(expected)
                emit(packet.data, packet.length)
            }
        }

        private fun skipMissingUntilReady(emit: (ByteArray, Int) -> Unit) {
            while (pending.size >= maxPending) {
                val expected = expectedSeq ?: return
                expectedSeq = next(expected)
                if (pending.containsKey(expectedSeq)) {
                    drain(emit)
                    return
                }
            }
        }

        private fun sequence(data: ByteArray): Int =
            ((data[2].toInt() and 0xff) shl 8) or (data[3].toInt() and 0xff)

        private fun next(seq: Int): Int = (seq + 1) and 0xffff

        private fun sequenceDistance(seq: Int, expected: Int): Int {
            val diff = (seq - expected) and 0xffff
            return if (diff < 32768) diff else diff - 65536
        }

        private data class PendingPacket(val data: ByteArray, val length: Int)
    }

    companion object {
        private const val TAG = "RtpVideoPlayer"
        private const val CODEC_RETRY_DELAY_MS = 500L
        private const val REORDER_BUFFER_PACKETS = 16
        private const val RTP_HEADER_MIN_BYTES = 12
    }
}
