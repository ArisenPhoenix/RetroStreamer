#pragma once

#include "archstreamer/runtime_cadence/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Tracks one live session's identity + resource claims in cadence.
 * Soft-fails; never throws into the play path.
 */
class CadenceSessionTracker {
public:
    CadenceSessionTracker() = default;

    [[nodiscard]] const std::string& session_id() const { return session_id_; }
    [[nodiscard]] bool active() const { return !session_id_.empty(); }

    /** Create session row and claim the initial fixed resources for this slot. */
    void begin(
        int slot,
        std::string_view username,
        std::string_view game_key,
        std::string_view system_key,
        std::string_view mode,
        std::string_view display,
        std::uint16_t video_port,
        std::uint16_t audio_port,
        std::uint16_t netcmd_port,
        std::string_view pulse_sink,
        std::string_view pulse_app_id,
        std::uint16_t pad_product_base);

    void claim(std::string_view resource_type, std::string_view resource_name, std::string_view detail = {});
    void claim_emulator_pid(int pid);

    /** Release all claims and mark the session ended. */
    void end(std::string_view end_reason);

private:
    std::string session_id_;
    std::string host_id_;
    int slot_ = -1;
};

} // namespace archstreamer
