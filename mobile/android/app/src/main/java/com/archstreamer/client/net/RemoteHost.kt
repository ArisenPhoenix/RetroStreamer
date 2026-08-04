package com.archstreamer.client.net

import com.archstreamer.client.protocol.ActiveSessionInfo
import com.archstreamer.client.protocol.IncomingPacket
import com.archstreamer.client.protocol.PacketCodec
import com.archstreamer.client.protocol.Protocol
import net.schmizz.sshj.SSHClient
import net.schmizz.sshj.connection.channel.direct.Session
import net.schmizz.sshj.transport.verification.PromiscuousVerifier
import org.bouncycastle.jce.provider.BouncyCastleProvider
import java.security.Security
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Port block for remote overflow instance index n (n >= 0). */
data class RemoteHostPortBlock(
    val controlPort: Int,
    val inputPort: Int,
    val videoPort: Int,
    val audioPort: Int,
    val virtualDisplay: Int,
    val instanceIndex: Int,
)

object RemoteHost {
    const val DEFAULT_SSH_PORT = 22
    const val DEFAULT_VIRTUAL_DISPLAY = 99

    private val bouncyCastleReady = AtomicBoolean(false)

    /**
     * Android ships a truncated BouncyCastle as provider "BC" that cannot do X25519.
     * Replace it with the full bcprov jar before any sshj handshake.
     */
    fun ensureBouncyCastle() {
        if (bouncyCastleReady.get()) return
        synchronized(bouncyCastleReady) {
            if (bouncyCastleReady.get()) return
            Security.removeProvider(BouncyCastleProvider.PROVIDER_NAME)
            Security.insertProviderAt(BouncyCastleProvider(), 1)
            bouncyCastleReady.set(true)
        }
    }

    fun portBlock(
        instanceIndex: Int,
        baseControl: Int = Protocol.DEFAULT_CONTROL_PORT,
        baseInput: Int = Protocol.DEFAULT_INPUT_PORT,
        baseVideo: Int = Protocol.DEFAULT_VIDEO_PORT,
        baseAudio: Int = Protocol.DEFAULT_AUDIO_PORT,
        baseDisplay: Int = DEFAULT_VIRTUAL_DISPLAY,
    ): RemoteHostPortBlock {
        val n = instanceIndex.coerceAtLeast(0)
        val offset = 10 * n
        return RemoteHostPortBlock(
            controlPort = baseControl + offset,
            inputPort = baseInput + offset,
            videoPort = baseVideo + offset,
            audioPort = baseAudio + offset,
            virtualDisplay = baseDisplay + n,
            instanceIndex = n,
        )
    }

    fun pidFilename(controlPort: Int): String = ".archstreamer_remote_$controlPort.pid"

    fun pidPath(directory: String, controlPort: Int): String {
        val dir = directory.trimEnd('/')
        return if (dir.isEmpty()) pidFilename(controlPort) else "$dir/${pidFilename(controlPort)}"
    }

    fun logPath(directory: String, controlPort: Int): String {
        val name = "host_$controlPort.log"
        val dir = directory.trimEnd('/')
        return if (dir.isEmpty()) name else "$dir/$name"
    }

    fun resolveBinary(directory: String, binary: String): String {
        var trimmed = binary.trim().trimEnd('/', '\\')
        val dir = directory.trim().trimEnd('/', '\\')

        fun joinDir(leaf: String): String =
            if (dir.isEmpty()) leaf else "$dir/$leaf"

        if (trimmed.isEmpty() || trimmed == "." || trimmed == "./") {
            return joinDir("host_runner")
        }

        // Absolute path already.
        if (trimmed.startsWith("/")) {
            val leaf = trimmed.substringAfterLast('/')
            if (leaf == "build" || leaf == "bin" || leaf == "Debug" || leaf == "Release") {
                return "$trimmed/host_runner"
            }
            return trimmed
        }

        if (trimmed.startsWith("./")) {
            trimmed = trimmed.removePrefix("./")
        }

        val leaf = trimmed.substringAfterLast('/').substringAfterLast('\\')
        if (leaf == "build" || leaf == "bin" || leaf == "Debug" || leaf == "Release") {
            return joinDir("$trimmed/host_runner")
        }
        return joinDir(trimmed)
    }

    fun lobbyFull(info: ActiveSessionInfo): Boolean {
        val active = info.activeSlots ?: return false
        val max = info.maxSlots ?: return false
        if (max == 0) return false
        return active >= max
    }

    data class GpuOption(val id: String, val name: String)

    /**
     * Fuzzy match remoting preference against host_runner --list-gpus options
     * (same rules as desktop Settings / resolve_render_gpu).
     */
    fun matchGpuOption(options: List<GpuOption>, want: String): GpuOption? {
        val trimmed = want.trim()
        if (trimmed.isEmpty() || trimmed.equals("auto", ignoreCase = true)) return null
        options.firstOrNull { it.id == trimmed }?.let { return it }
        fun normalize(value: String): String =
            value.lowercase().map { ch ->
                when (ch) {
                    ':', '_', '-', '/' -> ' '
                    else -> ch
                }
            }.joinToString("")
        val needle = normalize(trimmed)
        options.firstOrNull { option ->
            val hay = normalize("${option.id} ${option.name}")
            if (hay.contains(needle)) return@firstOrNull true
            val tokens = needle.split(Regex("\\s+")).filter { it.isNotEmpty() }
            tokens.isNotEmpty() && tokens.all { hay.contains(it) }
        }?.let { return it }
        return null
    }

    fun parseListGpusOutput(stdout: String): List<GpuOption> =
        stdout.lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .mapNotNull { line ->
                val tab = line.indexOf('\t')
                if (tab <= 0) return@mapNotNull null
                GpuOption(
                    id = line.substring(0, tab).trim(),
                    name = line.substring(tab + 1).trim(),
                )
            }
            .toList()

    fun listGpusShell(directory: String, binary: String): String {
        val qbin = shellSingleQuote(resolveBinary(directory, binary))
        return "set -e; " +
            "if [ ! -x $qbin ]; then echo \"host_runner not found or not executable: \" $qbin >&2; exit 127; fi; " +
            "$qbin --list-gpus"
    }

    /** Print --gpu from host_runner argv for this control port (or empty). */
    fun encodeGpuQueryShell(controlPort: Int): String =
        "found=\"\"; " +
            "for f in /proc/[0-9]*/cmdline; do " +
            "cmd=\$(tr '\\0' ' ' < \"\$f\" 2>/dev/null) || continue; " +
            "case \"\$cmd\" in *host_runner*) ;; *) continue ;; esac; " +
            "echo \"\$cmd\" | grep -Eq -- '--control-port[= ]*$controlPort([[:space:]]|\$)' || continue; " +
            "found=\$(echo \"\$cmd\" | sed -n 's/.*--gpu[= ]\\([^[:space:]]*\\).*/\\1/p'); " +
            "break; " +
            "done; " +
            "printf '%s\\n' \"\$found\""

    /**
     * Whether a free lobby may be reused for [wantResolvedId].
     * Blank want → any free lobby. Process without --gpu → host default (first list entry).
     */
    fun lobbyUsableForGpu(
        info: ActiveSessionInfo,
        wantResolvedId: String,
        processGpuArg: String,
        gpuOptions: List<GpuOption>,
    ): Boolean {
        if (lobbyFull(info)) return false
        if (wantResolvedId.isEmpty()) return true
        if (processGpuArg.isBlank() || processGpuArg.equals("auto", ignoreCase = true)) {
            return gpuOptions.isNotEmpty() && gpuOptions.first().id == wantResolvedId
        }
        val processMatch = matchGpuOption(gpuOptions, processGpuArg) ?: return false
        return processMatch.id == wantResolvedId
    }

    /**
     * Absolute script path, or relative to [directory]. Blank → Path A (host_runner).
     */
    fun resolveStartScript(directory: String, startScript: String): String {
        var trimmed = startScript.trim().trimEnd('/', '\\')
        if (trimmed.isEmpty()) return ""
        if (trimmed.startsWith("/")) return trimmed
        if (trimmed.startsWith("./")) trimmed = trimmed.removePrefix("./")
        val dir = directory.trim().trimEnd('/', '\\')
        return if (dir.isEmpty()) trimmed else "$dir/$trimmed"
    }

    /**
     * Path A ([startScript] blank): nohup host_runner with rom-root, ports, clients, GPU.
     * Path B ([startScript] set): nohup that script with ports + GPU only.
     */
    fun startShell(
        directory: String,
        binary: String,
        romRoot: String,
        ports: RemoteHostPortBlock,
        encodeGpu: String = "",
        startScript: String = "",
    ): String {
        val dir = directory.trim().trimEnd('/', '\\').ifEmpty { "." }
        val qdir = shellSingleQuote(dir)
        val qlog = shellSingleQuote(logPath(directory, ports.controlPort))
        val qpid = shellSingleQuote(pidPath(directory, ports.controlPort))
        val gpuArgs = if (encodeGpu.isNotBlank()) {
            " --gpu ${shellSingleQuote(encodeGpu.trim())}"
        } else {
            ""
        }
        val portArgs =
            " --control-port ${ports.controlPort}" +
                " --input-port ${ports.inputPort}" +
                " --video-port ${ports.videoPort}" +
                " --audio-port ${ports.audioPort}" +
                " --virtual-display :${ports.virtualDisplay}"
        val resolvedScript = resolveStartScript(directory, startScript)
        val useScript = resolvedScript.isNotEmpty()
        val launchTarget = if (useScript) resolvedScript else resolveBinary(directory, binary)
        val qlaunch = shellSingleQuote(launchTarget)
        val missingLabel = if (useScript) "start script" else "host_runner"
        val exitLabel = missingLabel
        val launchArgs = if (useScript) {
            portArgs + gpuArgs
        } else {
            " --rom-root ${shellSingleQuote(romRoot)}" +
                portArgs +
                " --clients 2 --allow-new-users" +
                gpuArgs
        }
        return "set -e; " +
            "mkdir -p $qdir; " +
            "if [ ! -x $qlaunch ]; then echo \"$missingLabel not found or not executable: \" $qlaunch >&2; exit 127; fi; " +
            "nohup $qlaunch" +
            launchArgs +
            " > $qlog 2>&1 & " +
            "pid=\$!; echo \"\$pid\" > $qpid; " +
            "sleep 0.7; " +
            "if ! kill -0 \"\$pid\" 2>/dev/null; then " +
            "echo \"$exitLabel exited immediately (pid \$pid). Log:\" >&2; " +
            "cat $qlog >&2 || true; rm -f $qpid; exit 1; fi"
    }

    /**
     * Stop by control-port match. Directory is optional (only used for PID-file cleanup).
     */
    fun stopShell(controlPort: Int, directory: String = ""): String {
        val parts = mutableListOf<String>()
        if (directory.isNotBlank()) {
            val qpid = shellSingleQuote(pidPath(directory, controlPort))
            parts += "if [ -f $qpid ]; then" +
                " pid=\$(cat $qpid);" +
                " kill \"\$pid\" 2>/dev/null;" +
                " sleep 1;" +
                " kill -0 \"\$pid\" 2>/dev/null && kill -9 \"\$pid\" 2>/dev/null;" +
                " rm -f $qpid; fi"
        }
        // [h] avoids pkill matching itself.
        parts += "pkill -f '[h]ost_runner.*--control-port[= ]*$controlPort' >/dev/null 2>&1 || true"
        parts += "sleep 1"
        parts += "pkill -9 -f '[h]ost_runner.*--control-port[= ]*$controlPort' >/dev/null 2>&1 || true"
        return parts.joinToString("; ")
    }

    fun shellSingleQuote(value: String): String =
        "'" + value.replace("'", "'\\''") + "'"

    /** Probe ActiveSessionInfo; null if unreachable. */
    fun probeActiveSession(host: String, controlPort: Int, timeoutMs: Int = 3_000): ActiveSessionInfo? {
        return try {
            ControlConnection(host, controlPort).use { conn ->
                conn.connect(timeoutMs)
                conn.send(PacketCodec.activeSessionInfoRequest())
                when (val packet = conn.receive()) {
                    is IncomingPacket.ActiveSession -> packet.value
                    else -> null
                }
            }
        } catch (_: Exception) {
            null
        }
    }

    data class SshResult(val ok: Boolean, val output: String, val error: String)

    private fun formatThrowable(error: Throwable): String = buildString {
        append(error.javaClass.simpleName)
        val message = error.message
        if (!message.isNullOrBlank()) {
            append(": ")
            append(message)
        }
        var cause = error.cause
        var depth = 0
        while (cause != null && depth < 6) {
            append("\nCaused by: ")
            append(cause.javaClass.simpleName)
            val causeMessage = cause.message
            if (!causeMessage.isNullOrBlank()) {
                append(": ")
                append(causeMessage)
            }
            cause = cause.cause
            depth++
        }
    }

    fun runSshCommand(
        host: String,
        port: Int,
        username: String,
        password: String,
        command: String,
        timeoutSec: Long = 60,
    ): SshResult {
        ensureBouncyCastle()
        val ssh = SSHClient()
        return try {
            ssh.addHostKeyVerifier(PromiscuousVerifier())
            ssh.connectTimeout = 15_000
            ssh.connect(host, port)
            ssh.authPassword(username, password)
            ssh.startSession().use { session ->
                val cmd: Session.Command = session.exec(command)
                val stdout = cmd.inputStream.bufferedReader().readText()
                val stderr = cmd.errorStream.bufferedReader().readText()
                cmd.join(timeoutSec, TimeUnit.SECONDS)
                val code = cmd.exitStatus ?: -1
                SshResult(
                    ok = code == 0,
                    output = stdout,
                    error = if (code == 0) {
                        ""
                    } else {
                        stderr.ifBlank { "ssh exit $code" }.let { err ->
                            if (stdout.isBlank()) err else "$err\n$stdout"
                        }
                    },
                )
            }
        } catch (error: Exception) {
            SshResult(ok = false, output = "", error = formatThrowable(error))
        } finally {
            try {
                ssh.disconnect()
            } catch (_: Exception) {
            }
        }
    }
}
