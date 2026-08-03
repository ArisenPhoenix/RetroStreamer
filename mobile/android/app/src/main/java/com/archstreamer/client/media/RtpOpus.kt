package com.archstreamer.client.media

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Strip RTP headers from Opus packets (RFC 7587 / GStreamer rtpopuspay pt=97).
 * Returns the Opus payload bytes, or null if the datagram is not usable.
 */
object RtpOpusDepayloader {
    fun payload(packet: ByteArray, length: Int): ByteArray? {
        if (length < 12) return null
        val version = (packet[0].toInt() ushr 6) and 0x3
        if (version != 2) return null
        val padding = (packet[0].toInt() and 0x20) != 0
        val extension = (packet[0].toInt() and 0x10) != 0
        val cc = packet[0].toInt() and 0x0f
        var offset = 12 + cc * 4
        if (extension) {
            if (offset + 4 > length) return null
            val extLen = ((packet[offset + 2].toInt() and 0xff) shl 8) or
                (packet[offset + 3].toInt() and 0xff)
            offset += 4 + extLen * 4
        }
        var end = length
        if (padding && end > offset) {
            val pad = packet[end - 1].toInt() and 0xff
            end -= pad
        }
        if (offset >= end) return null
        return packet.copyOfRange(offset, end)
    }
}

/** RFC 7845 Opus identification header for MediaCodec csd-0. */
fun opusHeadCsd(channelCount: Int = 2, sampleRate: Int = 48_000): ByteBuffer {
    val buf = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN)
    buf.put("OpusHead".toByteArray(Charsets.US_ASCII))
    buf.put(1) // version
    buf.put(channelCount.toByte())
    buf.putShort(0) // pre-skip
    buf.putInt(sampleRate)
    buf.putShort(0) // output gain
    buf.put(0) // mapping family 0
    buf.flip()
    return buf
}

/** MediaCodec Opus requires csd-1/csd-2 as native-order uint64 nanosecond values. */
fun opusDelayCsd(nanos: Long): ByteBuffer =
    ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).putLong(nanos).also { it.flip() }

/** Common 80 ms seek pre-roll used by Android's Opus MediaFormat samples. */
const val OPUS_SEEK_PRE_ROLL_NS = 80_000_000L
