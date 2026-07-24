#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace archstreamer {

// Default RetroArch network_cmd_port used for ArchStreamer sessions (localhost only).
constexpr std::uint16_t DefaultRetroArchNetcmdPort = 55355;

// Fire-and-forget UDP command to RetroArch's network control interface.
bool send_retroarch_netcmd(
    std::string_view command,
    std::uint16_t port = DefaultRetroArchNetcmdPort,
    std::string_view host = "127.0.0.1");

} // namespace archstreamer
