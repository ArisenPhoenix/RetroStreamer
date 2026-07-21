#include "common/addresses.hpp"

#include <stdexcept>

namespace archstreamer {

std::string rtp_h264_uri(std::string host, std::uint16_t port) {
    return std::string{RtpH264Scheme} + host + ':' + std::to_string(port);
}

std::string rtp_opus_uri(std::string host, std::uint16_t port) {
    return std::string{RtpOpusScheme} + host + ':' + std::to_string(port);
}

std::uint16_t port_from_rtp_uri(std::string_view uri, std::string_view expected_scheme) {
    if (uri.rfind(expected_scheme, 0) != 0) {
        throw std::runtime_error("unsupported media endpoint: " + std::string{uri});
    }

    const auto colon = uri.rfind(':');
    if (colon == std::string_view::npos || colon + 1 >= uri.size()) {
        throw std::runtime_error("media endpoint is missing a port: " + std::string{uri});
    }

    return static_cast<std::uint16_t>(std::stoul(std::string{uri.substr(colon + 1)}));
}

std::uint16_t video_port_from_endpoint(const MediaEndpoint& endpoint) {
    return port_from_rtp_uri(endpoint.video_uri, RtpH264Scheme);
}

std::uint16_t audio_port_from_endpoint(const MediaEndpoint& endpoint) {
    return port_from_rtp_uri(endpoint.audio_uri, RtpOpusScheme);
}

} // namespace archstreamer
