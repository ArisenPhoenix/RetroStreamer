#pragma once

#include <cstdint>
#include <string>

namespace archstreamer {

/**
 * Machine-wide exclusive claim on a concurrent session slot number.
 *
 * The slot number drives the X display, RTP port block, streaming sink name and
 * virtual pad product ids. A per-process counter is not enough: a second host
 * on the same machine restarts at 0 and silently shares those resources with a
 * live session — its emulator lands on the other host's display, so both games
 * end up in that host's capture.
 */
class SessionSlotLease {
public:
    SessionSlotLease() = default;
    ~SessionSlotLease();

    SessionSlotLease(const SessionSlotLease&) = delete;
    SessionSlotLease& operator=(const SessionSlotLease&) = delete;

    SessionSlotLease(SessionSlotLease&& other) noexcept;
    SessionSlotLease& operator=(SessionSlotLease&& other) noexcept;

    /**
     * Lowest slot in [0, span) that no other host process holds and whose X
     * display (display_base + slot) has no server. Invalid when all are taken.
     */
    static SessionSlotLease claim(int span, int display_base);

    bool valid() const { return index_ >= 0; }
    int index() const { return index_; }
    void release();

private:
    SessionSlotLease(int index, std::intptr_t handle)
        : index_(index)
        , handle_(handle) {}

    int index_ = -1;
    std::intptr_t handle_ = -1;
};

namespace slot_lock {

/** Directory holding the slot lock files; created on demand. */
std::string lock_directory();

/** Exclusive non-blocking lock; -1 when another process already holds it. */
std::intptr_t try_acquire(const std::string& path);

void release(std::intptr_t handle);

/** True when no X server owns this display number (always true on Windows). */
bool display_number_free(int display_number);

/**
 * Unlink leftover gamescope X reservation lock files (`/tmp/.Xn-lock`, n>=1)
 * that have no matching `/tmp/.X11-unix/Xn` socket.
 *
 * The gamescope wrapper creates those locks without a filesystem socket so
 * nested Xwayland lands on `:20`. SIGKILL of the helper skips atexit, so the
 * files linger and the next session can fail to open a capture display.
 * Skipped while any `ARCHSTREAMER_SLOT_TOKEN` process is still alive so a
 * sibling slot keeps its reservations. No-op on Windows.
 */
void release_stale_x11_reservation_locks();

} // namespace slot_lock

} // namespace archstreamer
