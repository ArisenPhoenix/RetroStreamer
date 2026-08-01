#include "host/session_slot_lease.hpp"

#include <utility>

namespace archstreamer {

SessionSlotLease::~SessionSlotLease() {
    release();
}

SessionSlotLease::SessionSlotLease(SessionSlotLease&& other) noexcept
    : index_(std::exchange(other.index_, -1))
    , handle_(std::exchange(other.handle_, -1)) {}

SessionSlotLease& SessionSlotLease::operator=(SessionSlotLease&& other) noexcept {
    if (this != &other) {
        release();
        index_ = std::exchange(other.index_, -1);
        handle_ = std::exchange(other.handle_, -1);
    }
    return *this;
}

void SessionSlotLease::release() {
    if (handle_ != -1) {
        slot_lock::release(handle_);
    }
    handle_ = -1;
    index_ = -1;
}

SessionSlotLease SessionSlotLease::claim(int span, int display_base) {
    const auto directory = slot_lock::lock_directory();
    for (int index = 0; index < span; ++index) {
        const auto path = directory + "/slot-" + std::to_string(index) + ".lock";
        const auto handle = slot_lock::try_acquire(path);
        if (handle == -1) {
            continue;
        }
        // A wiped lock directory (reboot, tmp cleanup) can hand out a slot whose
        // display still belongs to a live host, which is the exact collision the
        // lease exists to prevent.
        if (!slot_lock::display_number_free(display_base + index)) {
            slot_lock::release(handle);
            continue;
        }
        return SessionSlotLease(index, handle);
    }
    return {};
}

} // namespace archstreamer
