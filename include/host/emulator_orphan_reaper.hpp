#pragma once

namespace archstreamer {

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

} // namespace archstreamer
