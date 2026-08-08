#pragma once

#include <string>
#include <vector>

namespace archstreamer {

/**
 * True if another host_runner process (not this pid) is alive on this machine.
 * Used so a second GPU host does not wipe the shared Users/Remote presence dir.
 */
bool other_host_runner_alive();

/**
 * A live host_runner, reduced to the launch arguments that identify which
 * configuration it belongs to.
 */
struct HostRunnerProcess {
    int pid = 0;
    /** --control-port value; 0 when the process did not name one. */
    int control_port = 0;
    /** --gpu value; empty when the process did not name one. */
    std::string gpu;
    /**
     * PID of the live archstreamer_gui in its ancestry, or 0 when nothing
     * supervises it. Only a 0 here marks a genuine leftover: a second GUI with
     * the same settings otherwise looks identical to a host that is still owned
     * and streaming to a client.
     */
    int owner_gui_pid = 0;
};

/**
 * Live host_runner processes, skipping `ignore_pid` (pass the one the caller
 * already owns, or 0 for none).
 *
 * A GUI that dies without running its destructor never stops its host_runner,
 * which keeps holding the control port; the next Start Host then fails to bind
 * and Stop Host cannot help, because the leftover belongs to no live QProcess.
 * Reporting the control port, GPU, and owning GUI lets a caller tell its own
 * leftover apart from a host it must not touch.
 */
std::vector<HostRunnerProcess> list_host_runner_processes(int ignore_pid = 0);

/**
 * Stop a host_runner this process does not own: SIGTERM, then SIGKILL if it is
 * still alive after `grace_ms`. host_runner polls a stop flag cooperatively, so
 * a wedged run loop needs the escalation. Returns true once the pid is gone.
 */
bool terminate_host_runner(int pid, int grace_ms = 3000);

/**
 * Last-resort cleanup for emulator trees left behind when a previous host
 * process died without running SessionRuntime / HostRetroArchProcess destructors
 * (crash, kill -9, etc.).
 *
 * Normal session teardown must stop processes via SessionRuntime ownership —
 * do not use this as the primary policy. Call once at host start before any
 * new session launches.
 *
 * If another host_runner is already alive, this is a no-op so a second GUI/host
 * cannot tear down someone else's session (including after a rebuild where
 * /proc/<pid>/exe shows "host_runner (deleted)").
 *
 * Orphans keep feeding their old archstreamer-<slot> sink, mixing that game's
 * audio under the next session. Trees still owned by a live host are left alone.
 *
 * Returns the number of orphan roots that were terminated.
 */
int reap_orphaned_emulator_processes();

/**
 * Track tokens injected as ARCHSTREAMER_SLOT_TOKEN by PosixRetroArchProcess.
 * Used so mid-lobby cleanup can kill abandoned trees without touching live slots.
 */
void register_emulator_session_token(const std::string& token);
void unregister_emulator_session_token(const std::string& token);

/**
 * Kill processes whose environ still carries ARCHSTREAMER_SLOT_TOKEN but the
 * token is no longer registered (failed stop / AppImage re-exec leftovers).
 * Safe while this host_runner has live sessions. Returns roots terminated.
 */
int reap_stale_emulator_session_tokens();

} // namespace archstreamer
