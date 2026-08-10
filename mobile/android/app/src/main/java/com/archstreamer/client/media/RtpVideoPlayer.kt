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
import java.util.ArrayDeque
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Binds the host-assigned RTP video UDP port, depayloads H.264, and decodes to a Surface.
 * Call [startReceiving] as soon as MediaEndpoint arrives (before the Surface exists);
 * NALs queue until [attachSurface] configures MediaCodec.
 *
 * Loss / corruption handling is intentionally split:
 * - Layer A (depay): drop until IDR after RTP gaps — never emit torn AUs.
 * - Layer B (player): soft-flush codec + hold inputs until IDR — clears poisoned
 *   buffers without destroying MediaCodec (green-bar prevention).
 * - Layer C (lifecycle): hard MediaCodec rebuild only on size change, configure
 *   failure, or escalated stall recovery (Amlogic dies if Layer B is folded into C).
 */
class RtpVideoPlayer(
    private val listenPort: Int,
    private val holdAfterFirstAccessUnit: Boolean = false,
    private val minGapPacketsForSoftFlush: Int = MIN_GAP_PACKETS_FOR_FLUSH,
    private val softFlushMinIntervalNs: Long = SOFT_FLUSH_MIN_INTERVAL_NS,
    private val reorderBufferPackets: Int = REORDER_BUFFER_PACKETS,
) : AutoCloseable {
    private val running = AtomicBoolean(false)
    private val framesDecoded = AtomicInteger(0)
    private val framesDecodedDelta = AtomicInteger(0)
    private val pipelineDead = AtomicBoolean(false)
    private val lastErrorMs = AtomicLong(0)

    private val nalQueue = ArrayBlockingQueue<RtpH264Depayloader.AccessUnit>(12)
    private val depay = RtpH264Depayloader()
    private val reorderBuffer = RtpReorderBuffer(reorderBufferPackets)
    private val accessUnitsReceived = AtomicInteger(0)
    private val keyframeAccessUnitsReceived = AtomicInteger(0)
    private val acceptingAccessUnits = AtomicBoolean(true)
    private val lastHeartbeatAccessUnits = AtomicInteger(0)
    private val decodeStallHeartbeats = AtomicInteger(0)
    private val softStallResyncs = AtomicInteger(0)
    private val queuedInputs = ArrayDeque<RtpH264Depayloader.AccessUnit>()
    /**
     * Bumps on every stream-integrity resync so an in-flight [drainDecode] cannot
     * feed pre-gap AUs into the codec after a soft flush was requested.
     */
    private val decodeGeneration = AtomicInteger(0)
    /** After a gap/flush, only IDR AUs may enter MediaCodec (avoids green bars). */
    private val decoderAwaitingKeyframe = AtomicBoolean(false)
    /** True once a non-IDR AU has been queued since the last IDR (codec may hold P-refs). */
    private val codecHasPredictiveRefs = AtomicBoolean(false)
    private val lastSoftFlushNs = AtomicLong(0)

    @Volatile private var surface: Surface? = null
    @Volatile private var codec: MediaCodec? = null
    @Volatile private var codecConfigured = false
    private val codecConfiguring = AtomicBoolean(false)
    private val codecResetRequested = AtomicBoolean(false)
    @Volatile private var sps: ByteArray? = null
    @Volatile private var pps: ByteArray? = null
    /** Surface identity currently configured into MediaCodec (avoid tear-down churn). */
    @Volatile private var attachedSurfaceIdentity: Int? = null

    /** Optional UI hook when the decoder reports a real frame size. */
    @Volatile var onVideoSize: ((width: Int, height: Int) -> Unit)? = null
    /** Optional hook after MediaCodec is bound to the current output Surface. */
    @Volatile var onOutputSurfaceAttached: (() -> Unit)? = null

    private var receiveThread: Thread? = null
    private var decodeThread: HandlerThread? = null
    private var decodeHandler: Handler? = null
    private var socket: DatagramSocket? = null

    // TEMP: frame pacing debug — remove when judder investigation is done.
    // Gate: debug APK + Settings → Debug → Log connections.
    private val paceAu = FramePaceWindow("au", listenPort)
    private val pacePresent = FramePaceWindow("present", listenPort)
    private val stageLatency = StageLatencyWindow(listenPort)
    @Volatile private var lastStageHealth = StageHealth()

    val port: Int get() = listenPort

    fun takeFramesDecodedDelta(): Int = framesDecodedDelta.getAndSet(0)

    /**
     * Heartbeat counters for the host Auto ladder.
     * [lossPermille] is RTP loss over the interval, or 1000 if the receive/decode
     * pipeline is dead (mirrors desktop gst receiver death).
     */
    fun takeHeartbeatStats(): HeartbeatStats {
        val frames = framesDecodedDelta.getAndSet(0)
        val auTotal = accessUnitsReceived.get()
        val auDelta = auTotal - lastHeartbeatAccessUnits.getAndSet(auTotal)
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
        val stageSnapshot = stageLatency.flushIfDue()
        if (stageSnapshot != null) {
            lastStageHealth = stageSnapshot.health
        }
        if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
            paceAu.flushIfDue()?.let { ClientFileLog.conn(it) }
            pacePresent.flushIfDue()?.let { ClientFileLog.conn(it) }
            stageSnapshot?.line?.let { ClientFileLog.conn(it) }
            if (auDelta > 0 && frames == 0 && !codecConfigured) {
                ClientFileLog.conn(
                    "video decode idle port=$listenPort " +
                        "surface=${surface != null} sps=${sps != null} pps=${pps != null} " +
                        "au_delta=$auDelta keyframes=${keyframeAccessUnitsReceived.get()}",
                )
            }
        }
        // Soft flush first when AUs still arrive but the decoder went blank.
        recoverDecoderStallIfNeeded(frames, auDelta)
        val health = lastStageHealth
        return HeartbeatStats(
            framesDecodedDelta = frames,
            lossPermille = loss,
            packetsReceived = packets.received,
            packetsLost = packets.lost,
            sequenceGaps = packets.sequenceGaps,
            pipelineDead = dead,
            decodeQueueP95Ms = health.decodeQueueP95Ms,
            decodeQueueMaxMs = health.decodeQueueMaxMs,
            auQueueP95Ms = health.auQueueP95Ms,
        )
    }

    /** True once RTP has delivered at least one H.264 access unit (no Surface needed). */
    fun hasReceivedAccessUnits(): Boolean = accessUnitsReceived.get() > 0 || sps != null

    /** True once this receiver has a complete IDR access unit queued or accepted. */
    fun hasReceivedKeyframeAccessUnit(): Boolean = keyframeAccessUnitsReceived.get() > 0

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
                        val resyncGap = depay.consumeResyncGap()
                        if (resyncGap != null) {
                            // Layer A already dropUntilIdr. Layer B only when the codec
                            // may already hold bad predictive refs — not on every blip.
                            reorderBuffer.reset()
                            depay.clearSequenceBaseline()
                            onStreamIntegrityResync(resyncGap)
                        }
                        if (au != null) {
                            if (maybeConfigureFromSpsPps(au)) {
                                nalQueue.clear()
                            }
                            offerNal(au)
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
        acceptingAccessUnits.set(true)
        val identity = System.identityHashCode(newSurface)
        if (surface === newSurface && attachedSurfaceIdentity == identity && codecConfigured) {
            onOutputSurfaceAttached?.invoke()
            return
        }
        val previous = surface
        surface = newSurface
        attachedSurfaceIdentity = identity
        if (ClientFileLog.logConnections) {
            ClientFileLog.conn(
                "video surface attach port=$listenPort " +
                    "sps=${sps != null} pps=${pps != null} configured=$codecConfigured",
            )
        }

        // Prefer a live output-surface swap so orientation / view resize does not
        // wait on a new IDR (full codec rebuild).
        val existing = codec
        if (existing != null && codecConfigured && previous != null && previous !== newSurface) {
            try {
                existing.setOutputSurface(newSurface)
                onOutputSurfaceAttached?.invoke()
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
        if (codecConfigured) {
            onOutputSurfaceAttached?.invoke()
        }
        scheduleDecode()
    }

    fun detachSurface() {
        if (surface != null && ClientFileLog.logConnections) {
            ClientFileLog.conn("video surface detach port=$listenPort")
        }
        surface = null
        attachedSurfaceIdentity = null
        codecConfigured = false
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        codec = null
        queuedInputs.clear()
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
        queuedInputs.clear()
    }

    private fun offerNal(au: RtpH264Depayloader.AccessUnit) {
        val hasIdr = containsNalType(au.data, 5)
        if (holdAfterFirstAccessUnit && (!hasIdr || !acceptingAccessUnits.get())) return
        accessUnitsReceived.incrementAndGet()
        if (hasIdr) {
            keyframeAccessUnitsReceived.incrementAndGet()
        }
        if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
            paceAu.record()
        }
        if (!nalQueue.offer(au)) {
            nalQueue.poll()
            nalQueue.offer(au)
        }
        if (holdAfterFirstAccessUnit) {
            acceptingAccessUnits.set(false)
        }
    }

    private fun containsNalType(data: ByteArray, wantedType: Int): Boolean {
        var i = 0
        while (i + 4 < data.size) {
            val startCodeLength = when {
                data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                    data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte() -> 4
                data[i] == 0.toByte() && data[i + 1] == 0.toByte() && data[i + 2] == 1.toByte() -> 3
                else -> 0
            }
            if (startCodeLength != 0) {
                val nalStart = i + startCodeLength
                if (nalStart < data.size && (data[nalStart].toInt() and 0x1f) == wantedType) {
                    return true
                }
                i = nalStart
            } else {
                i++
            }
        }
        return false
    }

    /**
     * Returns true when the caller should discard queued AUs that belonged to the
     * previous coded stream before enqueueing [au].
     */
    private fun maybeConfigureFromSpsPps(au: RtpH264Depayloader.AccessUnit): Boolean {
        var parameterSetsChanged = false
        var previousSps: ByteArray? = null
        val data = au.data
        var i = 0
        while (i + 4 < data.size) {
            if (data[i] == 0.toByte() && data[i + 1] == 0.toByte() &&
                data[i + 2] == 0.toByte() && data[i + 3] == 1.toByte()
            ) {
                val nalStart = i + 4
                if (nalStart >= data.size) break
                val type = data[nalStart].toInt() and 0x1f
                val next = nextStartCode(data, nalStart) ?: data.size
                val nal = data.copyOfRange(nalStart, next)
                when (type) {
                    7 -> {
                        val previous = sps
                        if (previous != null && !previous.contentEquals(nal)) {
                            parameterSetsChanged = true
                            previousSps = previous
                        }
                        sps = nal
                    }
                    8 -> {
                        val previous = pps
                        if (previous != null && !previous.contentEquals(nal)) {
                            parameterSetsChanged = true
                        }
                        pps = nal
                    }
                }
                i = next
            } else {
                i++
            }
        }
        if (parameterSetsChanged && codecConfigured) {
            // Ignore cosmetic SPS/PPS byte churn; only rebuild when coded size changes.
            val oldDims = previousSps?.let { parseSpsDimensions(it) }
            val newDims = sps?.let { parseSpsDimensions(it) }
            if (oldDims != null && newDims != null && oldDims != newDims) {
                requestCodecReset(clearParameterSets = false)
                return true
            }
        }
        if (codecResetRequested.get()) {
            return false
        }
        val s = sps
        val p = pps
        if (s != null && p != null && surface != null && !codecConfigured) {
            val lastError = lastErrorMs.get()
            if (lastError != 0L && System.currentTimeMillis() - lastError < CODEC_RETRY_DELAY_MS) {
                return false
            }
            configureCodec(s, p)
        }
        return false
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
            val dimensions = parseSpsDimensions(spsNal) ?: DEFAULT_VIDEO_DIMENSIONS
            val format = MediaFormat.createVideoFormat(
                MediaFormat.MIMETYPE_VIDEO_AVC,
                dimensions.width,
                dimensions.height,
            )
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
            onVideoSize?.invoke(dimensions.width, dimensions.height)
            Log.i(TAG, "MediaCodec configured for port $listenPort ${dimensions.width}x${dimensions.height}")
            if (ClientFileLog.logConnections) {
                ClientFileLog.conn(
                    "video codec configured port=$listenPort " +
                        "${dimensions.width}x${dimensions.height}",
                )
            }
        } catch (t: Throwable) {
            Log.e(TAG, "MediaCodec configure failed", t)
            if (ClientFileLog.logConnections) {
                ClientFileLog.conn(
                    "video codec configure failed port=$listenPort: ${t.message ?: t}",
                )
            }
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

    private fun parseSpsDimensions(nal: ByteArray): VideoDimensions? = runCatching {
        val rbsp = nalToRbsp(nal)
        if (rbsp.isEmpty()) return null
        val bits = BitReader(rbsp)
        bits.readBits(8) // NAL header.
        val profileIdc = bits.readBits(8)
        bits.readBits(8) // constraint flags + reserved bits.
        bits.readBits(8) // level_idc.
        bits.readUnsignedExpGolomb() // seq_parameter_set_id.

        var chromaFormatIdc = 1
        var separateColourPlaneFlag = false
        if (profileIdc in HIGH_PROFILE_IDS) {
            chromaFormatIdc = bits.readUnsignedExpGolomb()
            if (chromaFormatIdc == 3) {
                separateColourPlaneFlag = bits.readBit()
            }
            bits.readUnsignedExpGolomb() // bit_depth_luma_minus8.
            bits.readUnsignedExpGolomb() // bit_depth_chroma_minus8.
            bits.readBit() // qpprime_y_zero_transform_bypass_flag.
            val seqScalingMatrixPresent = bits.readBit()
            if (seqScalingMatrixPresent) {
                val scalingListCount = if (chromaFormatIdc != 3) 8 else 12
                repeat(scalingListCount) { index ->
                    if (bits.readBit()) {
                        skipScalingList(bits, if (index < 6) 16 else 64)
                    }
                }
            }
        }

        bits.readUnsignedExpGolomb() // log2_max_frame_num_minus4.
        val picOrderCntType = bits.readUnsignedExpGolomb()
        when (picOrderCntType) {
            0 -> bits.readUnsignedExpGolomb() // log2_max_pic_order_cnt_lsb_minus4.
            1 -> {
                bits.readBit() // delta_pic_order_always_zero_flag.
                bits.readSignedExpGolomb()
                bits.readSignedExpGolomb()
                repeat(bits.readUnsignedExpGolomb()) {
                    bits.readSignedExpGolomb()
                }
            }
        }
        bits.readUnsignedExpGolomb() // max_num_ref_frames.
        bits.readBit() // gaps_in_frame_num_value_allowed_flag.
        val picWidthInMbsMinus1 = bits.readUnsignedExpGolomb()
        val picHeightInMapUnitsMinus1 = bits.readUnsignedExpGolomb()
        val frameMbsOnlyFlag = bits.readBit()
        if (!frameMbsOnlyFlag) {
            bits.readBit() // mb_adaptive_frame_field_flag.
        }
        bits.readBit() // direct_8x8_inference_flag.

        var frameCropLeft = 0
        var frameCropRight = 0
        var frameCropTop = 0
        var frameCropBottom = 0
        if (bits.readBit()) {
            frameCropLeft = bits.readUnsignedExpGolomb()
            frameCropRight = bits.readUnsignedExpGolomb()
            frameCropTop = bits.readUnsignedExpGolomb()
            frameCropBottom = bits.readUnsignedExpGolomb()
        }

        val width = (picWidthInMbsMinus1 + 1) * 16
        val height = (picHeightInMapUnitsMinus1 + 1) * 16 * if (frameMbsOnlyFlag) 1 else 2
        val cropUnits = cropUnits(chromaFormatIdc, separateColourPlaneFlag, frameMbsOnlyFlag)
        val croppedWidth = width - (frameCropLeft + frameCropRight) * cropUnits.width
        val croppedHeight = height - (frameCropTop + frameCropBottom) * cropUnits.height
        if (croppedWidth > 0 && croppedHeight > 0) {
            VideoDimensions(croppedWidth, croppedHeight)
        } else {
            null
        }
    }.getOrNull()

    private fun nalToRbsp(nal: ByteArray): ByteArray {
        val start = when {
            nal.size >= 4 && nal[0] == 0.toByte() && nal[1] == 0.toByte() &&
                nal[2] == 0.toByte() && nal[3] == 1.toByte() -> 4
            nal.size >= 3 && nal[0] == 0.toByte() && nal[1] == 0.toByte() && nal[2] == 1.toByte() -> 3
            else -> 0
        }
        val out = ArrayList<Byte>(nal.size - start)
        var zeroCount = 0
        var i = start
        while (i < nal.size) {
            val b = nal[i]
            if (zeroCount >= 2 && b == 0x03.toByte()) {
                zeroCount = 0
                i++
                continue
            }
            out.add(b)
            zeroCount = if (b == 0.toByte()) zeroCount + 1 else 0
            i++
        }
        return out.toByteArray()
    }

    private fun cropUnits(
        chromaFormatIdc: Int,
        separateColourPlaneFlag: Boolean,
        frameMbsOnlyFlag: Boolean,
    ): VideoDimensions {
        val chromaArrayType = if (separateColourPlaneFlag) 0 else chromaFormatIdc
        val subWidthC: Int
        val subHeightC: Int
        when (chromaArrayType) {
            0 -> {
                subWidthC = 1
                subHeightC = 1
            }
            1 -> {
                subWidthC = 2
                subHeightC = 2
            }
            2 -> {
                subWidthC = 2
                subHeightC = 1
            }
            else -> {
                subWidthC = 1
                subHeightC = 1
            }
        }
        val cropUnitX = if (chromaArrayType == 0) 1 else subWidthC
        val cropUnitY = if (chromaArrayType == 0) {
            if (frameMbsOnlyFlag) 1 else 2
        } else {
            subHeightC * if (frameMbsOnlyFlag) 1 else 2
        }
        return VideoDimensions(cropUnitX, cropUnitY)
    }

    private fun skipScalingList(bits: BitReader, size: Int) {
        var lastScale = 8
        var nextScale = 8
        repeat(size) {
            if (nextScale != 0) {
                val deltaScale = bits.readSignedExpGolomb()
                nextScale = (lastScale + deltaScale + 256) % 256
            }
            lastScale = if (nextScale == 0) lastScale else nextScale
        }
    }

    private fun scheduleDecode() {
        val generation = decodeGeneration.get()
        decodeHandler?.post { drainDecode(generation) }
    }

    private fun drainDecode(generation: Int = decodeGeneration.get()) {
        if (generation != decodeGeneration.get()) return
        handlePendingCodecReset()
        if (!codecConfigured) {
            val s = sps
            val p = pps
            if (s != null && p != null && surface != null) {
                configureCodec(s, p)
            }
        }
        val c = codec
        if (c == null || !codecConfigured) return
        if (generation != decodeGeneration.get()) return
        try {
            drainCodecOutput(c)
            while (true) {
                if (generation != decodeGeneration.get()) return
                if (queuedInputs.size >= MAX_CODEC_IN_FLIGHT) {
                    trimPendingAccessUnits(MAX_PENDING_ACCESS_UNITS_WHEN_BACKED_UP)
                    decodeHandler?.postDelayed({ drainDecode(generation) }, 2)
                    return
                }

                val au = pollNextDecodableAccessUnit() ?: break
                val inIndex = c.dequeueInputBuffer(0)
                if (inIndex < 0) {
                    nalQueue.offer(au)
                    decodeHandler?.postDelayed({ drainDecode(generation) }, 2)
                    return
                }
                val input = c.getInputBuffer(inIndex) ?: continue
                input.clear()
                input.put(au.data)
                val queuedNs = System.nanoTime()
                val isIdr = containsNalType(au.data, 5)
                val flags = if (isIdr) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                c.queueInputBuffer(inIndex, 0, au.data.size, queuedNs / 1000, flags)
                if (isIdr) {
                    decoderAwaitingKeyframe.set(false)
                    codecHasPredictiveRefs.set(false)
                } else {
                    codecHasPredictiveRefs.set(true)
                }
                queuedInputs.addLast(au.copy(queuedNs = queuedNs))
                drainCodecOutput(c)
            }
            drainCodecOutput(c)
        } catch (t: Throwable) {
            Log.w(TAG, "decode error", t)
            codecConfigured = false
            pipelineDead.set(true)
            lastErrorMs.set(System.currentTimeMillis())
            nalQueue.clear()
            queuedInputs.clear()
            depay.resetUntilIdr()
            decoderAwaitingKeyframe.set(true)
            runCatching { c.stop() }
            runCatching { c.release() }
            codec = null
            return
        }
    }

    /**
     * While [decoderAwaitingKeyframe] is set, skip P/B AUs left in the queue so a
     * soft-flushed codec never sees a non-IDR first (green/tile corruption).
     */
    private fun pollNextDecodableAccessUnit(): RtpH264Depayloader.AccessUnit? {
        while (true) {
            val au = nalQueue.poll() ?: return null
            if (!decoderAwaitingKeyframe.get() || containsNalType(au.data, 5)) {
                return au
            }
        }
    }

    private fun recoverDecoderStallIfNeeded(frames: Int, accessUnits: Int) {
        if (frames > 0) {
            decodeStallHeartbeats.set(0)
            softStallResyncs.set(0)
            return
        }
        if (accessUnits <= 0 || !codecConfigured) {
            return
        }
        val streak = decodeStallHeartbeats.incrementAndGet()
        if (streak < DECODE_STALL_RESET_HEARTBEATS) {
            return
        }
        decodeStallHeartbeats.set(0)
        if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
            ClientFileLog.conn(
                "video decoder stall detected port=$listenPort " +
                    "startup=${!hasDecodedFrames()} au_delta=$accessUnits",
            )
        }
        resetDecoderAfterStall()
    }

    /**
     * Layer B — stream integrity hit the player.
     *
     * Always: drop queued AUs and hold decode until the next IDR (cheap; stops green
     * bars from feeding P-frames over a hole).
     *
     * Soft-flush MediaCodec only when it is likely already poisoned — predictive refs
     * in flight and a real gap — and at most once per [softFlushMinIntervalNs].
     * Flushing on every 1-packet Wi‑Fi blip caused top-of-screen bars ~1 Hz on TV.
     */
    private fun onStreamIntegrityResync(gapPackets: Int) {
        nalQueue.clear()
        decoderAwaitingKeyframe.set(true)
        val generation = decodeGeneration.incrementAndGet()

        val now = System.nanoTime()
        val nsSinceSoftFlush = now - lastSoftFlushNs.get()
        val dueByInterval = nsSinceSoftFlush >= softFlushMinIntervalNs
        val needsFlush = codecHasPredictiveRefs.get() &&
            gapPackets >= minGapPacketsForSoftFlush &&
            dueByInterval

        if (!needsFlush) {
            if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
                ClientFileLog.conn(
                    "video resync hold-idr port=$listenPort gap=$gapPackets " +
                        "flush=false refs=${codecHasPredictiveRefs.get()} " +
                        "since_flush_ms=${nsSinceSoftFlush / 1_000_000L} " +
                        "min_gap=$minGapPacketsForSoftFlush",
                )
            }
            return
        }

        lastSoftFlushNs.set(now)
        decodeHandler?.post {
            if (generation != decodeGeneration.get()) return@post
            queuedInputs.clear()
            val c = codec
            if (c != null && codecConfigured) {
                runCatching { c.flush() }
                codecHasPredictiveRefs.set(false)
                if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
                    ClientFileLog.conn(
                        "video decoder soft resync port=$listenPort " +
                            "gen=$generation gap=$gapPackets",
                    )
                }
            }
        }
    }

    private fun resetDecoderAfterStall() {
        decodeHandler?.post {
            nalQueue.clear()
            queuedInputs.clear()
            depay.resetUntilIdr(requestResync = false)
            decoderAwaitingKeyframe.set(true)
            val softCount = softStallResyncs.incrementAndGet()
            val c = codec
            if (softCount <= 2 && c != null && codecConfigured) {
                decodeGeneration.incrementAndGet()
                runCatching { c.flush() }
                if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
                    ClientFileLog.conn(
                        "video decoder stall soft-flush port=$listenPort n=$softCount",
                    )
                }
                return@post
            }
            softStallResyncs.set(0)
            if (BuildConfig.DEBUG && ClientFileLog.logConnections) {
                ClientFileLog.conn("video decoder stall hard-reset port=$listenPort")
            }
            // Layer C — codec lifecycle only after soft flushes failed to unstick.
            requestCodecReset(clearParameterSets = false)
        }
    }

    private fun drainCodecOutput(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        var outIndex = c.dequeueOutputBuffer(info, 0)
        while (outIndex != MediaCodec.INFO_TRY_AGAIN_LATER) {
            when {
                outIndex >= 0 -> {
                    val outputNs = System.nanoTime()
                    val source = if (queuedInputs.isEmpty()) null else queuedInputs.removeFirst()
                    c.releaseOutputBuffer(outIndex, true)
                    val releasedNs = System.nanoTime()
                    framesDecoded.incrementAndGet()
                    framesDecodedDelta.incrementAndGet()
                    pipelineDead.set(false)
                    if (source != null) {
                        stageLatency.record(source, outputNs, releasedNs)
                    }
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
    }

    private fun trimPendingAccessUnits(keepNewest: Int) {
        while (nalQueue.size > keepNewest) {
            nalQueue.poll()
        }
    }

    private fun requestCodecReset(clearParameterSets: Boolean) {
        if (clearParameterSets) {
            sps = null
            pps = null
        }
        codecResetRequested.set(true)
        scheduleDecode()
    }

    private fun handlePendingCodecReset() {
        if (!codecResetRequested.getAndSet(false)) return
        codecConfigured = false
        decoderAwaitingKeyframe.set(true)
        decodeGeneration.incrementAndGet()
        val old = codec
        codec = null
        runCatching { old?.stop() }
        runCatching { old?.release() }
        queuedInputs.clear()
    }

    data class HeartbeatStats(
        val framesDecodedDelta: Int,
        val lossPermille: Int,
        val packetsReceived: Long = 0,
        val packetsLost: Long = 0,
        val sequenceGaps: Long = 0,
        val pipelineDead: Boolean = false,
        val decodeQueueP95Ms: Int = LATENCY_UNKNOWN_MS,
        val decodeQueueMaxMs: Int = LATENCY_UNKNOWN_MS,
        val auQueueP95Ms: Int = LATENCY_UNKNOWN_MS,
    )

    data class StageHealth(
        val decodeQueueP95Ms: Int = LATENCY_UNKNOWN_MS,
        val decodeQueueMaxMs: Int = LATENCY_UNKNOWN_MS,
        val auQueueP95Ms: Int = LATENCY_UNKNOWN_MS,
    )

    private data class VideoDimensions(val width: Int, val height: Int)

    private class BitReader(private val data: ByteArray) {
        private var bitOffset = 0

        fun readBit(): Boolean = readBits(1) == 1

        fun readBits(count: Int): Int {
            require(count in 0..31)
            var value = 0
            repeat(count) {
                if (bitOffset >= data.size * 8) {
                    throw IllegalArgumentException("SPS ended unexpectedly")
                }
                val byte = data[bitOffset / 8].toInt() and 0xff
                val bit = (byte shr (7 - (bitOffset % 8))) and 1
                value = (value shl 1) or bit
                bitOffset++
            }
            return value
        }

        fun readUnsignedExpGolomb(): Int {
            var leadingZeros = 0
            while (!readBit()) {
                leadingZeros++
                if (leadingZeros > 31) {
                    throw IllegalArgumentException("Invalid Exp-Golomb code")
                }
            }
            val suffix = if (leadingZeros == 0) 0 else readBits(leadingZeros)
            return (1 shl leadingZeros) - 1 + suffix
        }

        fun readSignedExpGolomb(): Int {
            val codeNum = readUnsignedExpGolomb()
            val value = (codeNum + 1) / 2
            return if (codeNum % 2 == 0) -value else value
        }
    }

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
     * 1 Hz stage latency summary for TV decode/render profiling.
     *
     * recv_au = first RTP packet for AU -> complete AU assembled.
     * au_queue = complete AU -> queued into MediaCodec.
     * queue_out = MediaCodec queueInputBuffer -> dequeueOutputBuffer.
     * out_release = decoded output available -> releaseOutputBuffer(render=true).
     */
    private class StageLatencyWindow(
        private val port: Int,
    ) {
        private val lock = Any()
        private var windowStartNs = 0L
        private val recvToAuMs = ArrayList<Double>(64)
        private val auToQueueMs = ArrayList<Double>(64)
        private val queueToOutputMs = ArrayList<Double>(64)
        private val outputToReleaseMs = ArrayList<Double>(64)

        fun record(
            au: RtpH264Depayloader.AccessUnit,
            outputNs: Long,
            releasedNs: Long,
        ) {
            val queuedNs = au.queuedNs
            if (queuedNs == 0L) return
            synchronized(lock) {
                if (windowStartNs == 0L) {
                    windowStartNs = releasedNs
                }
                addMs(recvToAuMs, au.completedNs - au.firstPacketNs)
                addMs(auToQueueMs, queuedNs - au.completedNs)
                addMs(queueToOutputMs, outputNs - queuedNs)
                addMs(outputToReleaseMs, releasedNs - outputNs)
            }
        }

        fun flushIfDue(): StageSnapshot? {
            val now = System.nanoTime()
            synchronized(lock) {
                if (windowStartNs == 0L || now - windowStartNs < 1_000_000_000L) {
                    return null
                }
                if (queueToOutputMs.isEmpty()) {
                    clear(now)
                    return null
                }
                val recvToAu = summary(recvToAuMs)
                val auToQueue = summary(auToQueueMs)
                val queueToOutput = summary(queueToOutputMs)
                val outputToRelease = summary(outputToReleaseMs)
                val line = "stage port=$port n=${queueToOutputMs.size} " +
                    "recv_au_ms ${recvToAu.text} " +
                    "au_queue_ms ${auToQueue.text} " +
                    "queue_out_ms ${queueToOutput.text} " +
                    "out_release_ms ${outputToRelease.text}"
                val snapshot = StageSnapshot(
                    health = StageHealth(
                        decodeQueueP95Ms = queueToOutput.p95Ms,
                        decodeQueueMaxMs = queueToOutput.maxMs,
                        auQueueP95Ms = auToQueue.p95Ms,
                    ),
                    line = line,
                )
                clear(now)
                return snapshot
            }
        }

        private fun clear(nowNs: Long) {
            recvToAuMs.clear()
            auToQueueMs.clear()
            queueToOutputMs.clear()
            outputToReleaseMs.clear()
            windowStartNs = nowNs
        }

        private fun addMs(values: ArrayList<Double>, deltaNs: Long) {
            if (deltaNs <= 0L) return
            val ms = deltaNs / 1_000_000.0
            if (ms < 5_000.0) {
                values.add(ms)
            }
        }

        private fun summary(values: ArrayList<Double>): StageSummary {
            if (values.isEmpty()) return StageSummary("n=0")
            values.sort()
            val n = values.size
            val avg = values.sum() / n
            val p50 = values[n / 2]
            val p95 = values[min(n - 1, (n * 95) / 100)]
            val maxValue = values.last()
            return StageSummary(
                text = "avg=${"%.1f".format(avg)} p50=${"%.1f".format(p50)} " +
                    "p95=${"%.1f".format(p95)} max=${"%.1f".format(maxValue)}",
                p95Ms = toHeartbeatMs(p95),
                maxMs = toHeartbeatMs(maxValue),
            )
        }

        private fun toHeartbeatMs(value: Double): Int =
            value.roundToInt().coerceIn(0, LATENCY_UNKNOWN_MS - 1)

        data class StageSnapshot(
            val health: StageHealth,
            val line: String,
        )

        data class StageSummary(
            val text: String,
            val p95Ms: Int = LATENCY_UNKNOWN_MS,
            val maxMs: Int = LATENCY_UNKNOWN_MS,
        )
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

        fun reset() {
            pending.clear()
            expectedSeq = null
        }

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
        // Some Android TV decoders do not produce output with only 1-2 submitted AUs.
        // Keep a small prime window, but still cap it so a slow codec cannot build
        // the multi-second backlog seen before this path had backpressure.
        private const val MAX_CODEC_IN_FLIGHT = 4
        private const val MAX_PENDING_ACCESS_UNITS_WHEN_BACKED_UP = 2
        private const val DECODE_STALL_RESET_HEARTBEATS = 3
        private const val LATENCY_UNKNOWN_MS = 0xffff
        /** Soft-flush only for holes that likely poisoned predictive refs. */
        private const val MIN_GAP_PACKETS_FOR_FLUSH = 16
        /** Soft-flush at most every ~5s — more often paints top-of-screen bars on TV. */
        private const val SOFT_FLUSH_MIN_INTERVAL_NS = 5_000_000_000L
        private val DEFAULT_VIDEO_DIMENSIONS = VideoDimensions(1920, 1080)
        private val HIGH_PROFILE_IDS = setOf(
            100,
            110,
            122,
            244,
            44,
            83,
            86,
            118,
            128,
            138,
            139,
            134,
            135,
        )
    }
}
