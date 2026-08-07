#pragma once

// TEMP: frame pacing debug — remove when judder investigation is done.
// Gate: ARCHSTREAMER_DEBUG_FRAME_PACE=1
// Tees each encode to 127.0.0.1:<sniff> and logs 1 Hz RTP-marker Δt summaries.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace archstreamer::rtp_frame_pace_debug {

bool enabled();

/** Extra multiudpsink client for a copy of this encode's RTP. */
std::optional<std::pair<std::string, std::uint16_t>> ensure_tee(std::uint16_t encode_port);

void stop_tee(std::uint16_t encode_port);
void stop_all();

} // namespace archstreamer::rtp_frame_pace_debug
