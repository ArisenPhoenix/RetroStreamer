#include "host/cadence_session_events.hpp"

#include "archstreamer/runtime_cadence/cadence.hpp"

#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace archstreamer {

std::string cadence_host_id() {
#if defined(_WIN32)
    return std::to_string(static_cast<long long>(_getpid()));
#else
    return std::to_string(static_cast<long long>(::getpid()));
#endif
}

void record_cadence_event(cadence::RuntimeEvent event) {
    try {
        auto store = cadence::make_runtime_store();
        if (!store || !store->ensure_ready()) {
            return;
        }
        if (event.host_id.empty()) {
            event.host_id = cadence_host_id();
        }
        (void)store->record_event(event);
    } catch (...) {
        // Control-plane only — never fail a live session over logging.
    }
}

namespace {

void record_kind(
    std::string_view kind,
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail,
    std::string_view session_id) {
    cadence::RuntimeEvent event;
    event.kind = std::string(kind);
    event.slot = slot;
    event.username = std::string(username);
    event.game_key = std::string(game_key);
    event.detail = std::string(detail);
    event.session_id = std::string(session_id);
    record_cadence_event(std::move(event));
}

} // namespace

void record_session_started(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail,
    std::string_view session_id) {
    record_kind("session_started", slot, username, game_key, detail, session_id);
}

void record_session_ended(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail,
    std::string_view session_id) {
    record_kind("session_ended", slot, username, game_key, detail, session_id);
}

void record_client_joined(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail,
    std::string_view session_id) {
    record_kind("client_joined", slot, username, game_key, detail, session_id);
}

void record_client_left(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view detail,
    std::string_view session_id) {
    record_kind("client_left", slot, username, game_key, detail, session_id);
}

} // namespace archstreamer
