package com.archstreamer.client.media

import java.io.ByteArrayOutputStream
import java.util.concurrent.atomic.AtomicLong

/**
 * Minimal RTP H.264 depayloader (RFC 6184) for payload type 96 streams from ArchStreamer.
 * Emits Annex-B access units (start-code prefixed NALs) when the RTP marker bit is set,
 * or when a fragmented FU-A completes.
 *
 * Tracks RTP sequence gaps: incomplete FU-A is discarded and output is suppressed until
 * the next IDR so MediaCodec does not ingest a torn NAL.
 */
class RtpH264Depayloader {
    private val fuBuffer = ByteArrayOutputStream(64 * 1024)
    private var fuActive = false
    private val auBuffer = ByteArrayOutputStream(256 * 1024)
    private var lastSeq: Int? = null
    private var dropUntilIdr = false
    private val packetsReceived = AtomicLong(0)
    private val packetsLost = AtomicLong(0)
    /** Times an RTP sequence gap forced drop-until-IDR (green/tile risk until next keyframe). */
    private val sequenceGaps = AtomicLong(0)

    /** Packets accepted since the last [takePacketStats] call. */
    fun takePacketStats(): PacketStats {
        val received = packetsReceived.getAndSet(0)
        val lost = packetsLost.getAndSet(0)
        val gaps = sequenceGaps.getAndSet(0)
        return PacketStats(received = received, lost = lost, sequenceGaps = gaps)
    }

    fun push(packet: ByteArray, length: Int): ByteArray? {
        if (length < 12) return null
        val rtp = packet
        val version = (rtp[0].toInt() ushr 6) and 0x3
        if (version != 2) return null
        val padding = (rtp[0].toInt() and 0x20) != 0
        val extension = (rtp[0].toInt() and 0x10) != 0
        val cc = rtp[0].toInt() and 0x0f
        val marker = (rtp[1].toInt() and 0x80) != 0
        val seq = ((rtp[2].toInt() and 0xff) shl 8) or (rtp[3].toInt() and 0xff)
        noteSequence(seq)

        var offset = 12 + cc * 4
        if (extension) {
            if (offset + 4 > length) return null
            val extLen = ((rtp[offset + 2].toInt() and 0xff) shl 8) or (rtp[offset + 3].toInt() and 0xff)
            offset += 4 + extLen * 4
        }
        var end = length
        if (padding && end > offset) {
            val pad = rtp[end - 1].toInt() and 0xff
            end -= pad
        }
        if (offset >= end) return null

        packetsReceived.incrementAndGet()
        val nalType = rtp[offset].toInt() and 0x1f
        when {
            nalType in 1..23 -> {
                appendNal(rtp, offset, end - offset)
                if (marker) return flushAu()
            }
            nalType == 24 -> { // STAP-A
                var i = offset + 1
                while (i + 2 <= end) {
                    val size = ((rtp[i].toInt() and 0xff) shl 8) or (rtp[i + 1].toInt() and 0xff)
                    i += 2
                    if (i + size > end) break
                    appendNal(rtp, i, size)
                    i += size
                }
                if (marker) return flushAu()
            }
            nalType == 28 -> { // FU-A
                if (offset + 2 > end) return null
                val fuHeader = rtp[offset + 1].toInt() and 0xff
                val start = (fuHeader and 0x80) != 0
                val endBit = (fuHeader and 0x40) != 0
                val type = fuHeader and 0x1f
                if (start) {
                    fuBuffer.reset()
                    fuActive = true
                    val nalHeader = ((rtp[offset].toInt() and 0xe0) or type).toByte()
                    fuBuffer.write(nalHeader.toInt())
                }
                if (!fuActive) return null
                fuBuffer.write(rtp, offset + 2, end - offset - 2)
                if (endBit) {
                    val nal = fuBuffer.toByteArray()
                    fuActive = false
                    fuBuffer.reset()
                    appendNal(nal, 0, nal.size)
                    if (marker) return flushAu()
                }
            }
            else -> {
                // Skip unsupported aggregation / packetization modes.
            }
        }
        return null
    }

    private fun noteSequence(seq: Int) {
        val prev = lastSeq
        lastSeq = seq
        if (prev == null) return
        val expected = (prev + 1) and 0xffff
        if (seq == expected) return
        val gap = (seq - expected) and 0xffff
        // Ignore huge jumps (reset / reorder storm); still resync to IDR.
        if (gap in 1..4095) {
            packetsLost.addAndGet(gap.toLong())
        }
        sequenceGaps.incrementAndGet()
        if (fuActive) {
            fuActive = false
            fuBuffer.reset()
        }
        auBuffer.reset()
        dropUntilIdr = true
    }

    private fun appendNal(src: ByteArray, offset: Int, size: Int) {
        if (size <= 0) return
        val type = src[offset].toInt() and 0x1f
        if (dropUntilIdr) {
            when (type) {
                7, 8 -> Unit // keep parameter sets
                5 -> dropUntilIdr = false
                else -> return
            }
        }
        auBuffer.write(0)
        auBuffer.write(0)
        auBuffer.write(0)
        auBuffer.write(1)
        auBuffer.write(src, offset, size)
    }

    private fun flushAu(): ByteArray? {
        if (auBuffer.size() == 0) return null
        val out = auBuffer.toByteArray()
        auBuffer.reset()
        return out
    }

    data class PacketStats(val received: Long, val lost: Long, val sequenceGaps: Long = 0)
}

object MediaUris {
    const val H264_SCHEME = "rtp+h264://"
    const val OPUS_SCHEME = "rtp+opus://"

    fun portFrom(uri: String, scheme: String): Int {
        require(uri.startsWith(scheme)) { "unsupported media endpoint: $uri" }
        val colon = uri.lastIndexOf(':')
        require(colon > scheme.length && colon + 1 < uri.length) {
            "media endpoint is missing a port: $uri"
        }
        return uri.substring(colon + 1).toInt()
    }
}
