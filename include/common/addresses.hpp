#pragma once

#include "common/media.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace archstreamer {

inline constexpr std::string_view RtpH264Scheme = "rtp+h264://";
inline constexpr std::string_view RtpOpusScheme = "rtp+opus://";

std::string rtp_h264_uri(std::string host, std::uint16_t port);
std::string rtp_opus_uri(std::string host, std::uint16_t port);

std::uint16_t port_from_rtp_uri(std::string_view uri, std::string_view expected_scheme);
std::uint16_t video_port_from_endpoint(const MediaEndpoint& endpoint);
std::uint16_t audio_port_from_endpoint(const MediaEndpoint& endpoint);

} // namespace archstreamer
