#pragma once

#include "common/protocol.hpp"
#include "host/input_router.hpp"
#include "host/media_server.hpp"
#include "host/session_service.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace archstreamer {

class SessionControlMonitor {
public:
    SessionControlMonitor(
        SessionPlan& plan,
        InputRouter& input_router,
        MediaServer& media_server,
        std::chrono::seconds heartbeat_timeout,
        std::chrono::seconds reconnect_timeout);

    std::optional<std::string> poll();

private:
    bool remove_viewer(std::size_t index, std::string_view reason);
    void mark_player_disconnected(SessionClientConnection& client, std::string_view reason);
    void handle_heartbeat(SessionClientConnection& client, const ViewerHeartbeat& heartbeat);
    void apply_video_tier(SessionClientConnection& client, MediaQualityTier tier, std::string_view reason);
    static std::string client_label(const SessionClientConnection& client);

    SessionPlan& plan_;
    InputRouter& input_router_;
    MediaServer& media_server_;
    std::chrono::seconds heartbeat_timeout_;
    std::chrono::seconds reconnect_timeout_;
    std::chrono::steady_clock::time_point started_at_;
};

} // namespace archstreamer
