package com.archstreamer.client.media

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * RTP Opus (pt=97, 48 kHz stereo) → MediaCodec → AudioTrack.
 * Host streams `rtp+opus://host:port` via GStreamer rtpopuspay.
 */
class RtpOpusPlayer(
    private val listenPort: Int,
) : AutoCloseable {
    private val running = AtomicBoolean(false)
    private val packetsReceived = AtomicLong(0)
    private val framesDecoded = AtomicLong(0)

    private var socket: DatagramSocket? = null
    private var receiveThread: Thread? = null
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null

    val port: Int get() = listenPort
    fun packetsReceived(): Long = packetsReceived.get()
    fun framesDecoded(): Long = framesDecoded.get()

    fun start() {
        if (!running.compareAndSet(false, true)) return
        try {
            val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, 48_000, 2)
            // Android Opus needs all three CSD buffers (MediaCodec docs); csd-0 alone is silent
            // on many devices.
            format.setByteBuffer("csd-0", opusHeadCsd())
            format.setByteBuffer("csd-1", opusDelayCsd(0L))
            format.setByteBuffer("csd-2", opusDelayCsd(OPUS_SEEK_PRE_ROLL_NS))
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 4 * 1024)
            val decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS)
            decoder.configure(format, null, null, 0)
            decoder.start()
            codec = decoder

            val minBuf = AudioTrack.getMinBufferSize(
                48_000,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT,
            ).coerceAtLeast(48_000 * 2 * 2 / 10) // ~100 ms
            val audioTrack = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build(),
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(48_000)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .build(),
                )
                .setBufferSizeInBytes(minBuf * 2)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            audioTrack.play()
            track = audioTrack

            socket = DatagramSocket(null).apply {
                reuseAddress = true
                receiveBufferSize = 256 * 1024
                bind(InetSocketAddress(listenPort))
                soTimeout = 1000
            }

            receiveThread = Thread({
                val buf = ByteArray(1500)
                val packet = DatagramPacket(buf, buf.size)
                val info = MediaCodec.BufferInfo()
                var loggedFirst = false
                while (running.get()) {
                    try {
                        socket?.receive(packet) ?: break
                        val opus = RtpOpusDepayloader.payload(packet.data, packet.length) ?: continue
                        val n = packetsReceived.incrementAndGet()
                        if (!loggedFirst) {
                            loggedFirst = true
                            Log.i(TAG, "First Opus RTP packet on :$listenPort (${opus.size} B payload)")
                        } else if (n % 500L == 0L) {
                            Log.i(
                                TAG,
                                "Opus stats :$listenPort packets=$n decoded=${framesDecoded.get()}",
                            )
                        }
                        queueOpus(decoder, opus)
                        drainPcm(decoder, audioTrack, info)
                    } catch (_: java.net.SocketTimeoutException) {
                        drainPcm(decoder, audioTrack, info)
                    } catch (t: Throwable) {
                        if (running.get()) {
                            Log.w(TAG, "Opus receive/decode error on :$listenPort", t)
                        }
                    }
                }
            }, "rtp-opus-$listenPort").also {
                it.isDaemon = true
                it.start()
            }
            Log.i(TAG, "Opus audio listening on UDP :$listenPort")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to start Opus player on :$listenPort", t)
            close()
        }
    }

    private fun queueOpus(decoder: MediaCodec, opus: ByteArray) {
        val inIndex = decoder.dequeueInputBuffer(2_000)
        if (inIndex < 0) return
        val inBuf = decoder.getInputBuffer(inIndex) ?: return
        inBuf.clear()
        if (opus.size > inBuf.capacity()) {
            decoder.queueInputBuffer(inIndex, 0, 0, 0, 0)
            return
        }
        inBuf.put(opus)
        decoder.queueInputBuffer(
            inIndex,
            0,
            opus.size,
            System.nanoTime() / 1000L,
            0,
        )
    }

    private fun drainPcm(decoder: MediaCodec, audioTrack: AudioTrack, info: MediaCodec.BufferInfo) {
        while (true) {
            val outIndex = decoder.dequeueOutputBuffer(info, 0)
            when {
                outIndex == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                outIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    Log.i(TAG, "Opus output format: ${decoder.outputFormat}")
                }
                outIndex >= 0 -> {
                    val outBuf = decoder.getOutputBuffer(outIndex)
                    if (outBuf != null && info.size > 0 &&
                        (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0
                    ) {
                        val pcm = ByteArray(info.size)
                        outBuf.position(info.offset)
                        outBuf.get(pcm)
                        audioTrack.write(pcm, 0, pcm.size)
                        framesDecoded.incrementAndGet()
                    }
                    decoder.releaseOutputBuffer(outIndex, false)
                }
                else -> return
            }
        }
    }

    override fun close() {
        if (!running.getAndSet(false) && codec == null && track == null && socket == null) {
            return
        }
        running.set(false)
        runCatching { receiveThread?.join(500) }
        receiveThread = null
        runCatching { socket?.close() }
        socket = null
        runCatching {
            codec?.stop()
            codec?.release()
        }
        codec = null
        runCatching {
            track?.pause()
            track?.flush()
            track?.release()
        }
        track = null
    }

    companion object {
        private const val TAG = "RtpOpusPlayer"
    }
}
