#pragma once

#include <string>

namespace archstreamer {

/**
 * True if another host_runner process (not this pid) is alive on this machine.
 * Used so a second GPU host does not wipe the shared Users/Remote presence dir.
 */
bool other_host_runner_alive();

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
