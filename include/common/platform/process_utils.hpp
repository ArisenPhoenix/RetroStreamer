#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace archstreamer {

std::string trim_ascii_whitespace(std::string value);

/** Capture stdout of a shell command. On Windows, runs hidden (no console window). */
std::string read_command_output(const char* command);

/** Run a shell command; return process exit code (-1 on launch failure). Hidden on Windows. */
int run_command_exit_code(const char* command);

// Best-effort: stop leftover gst-launch publishers that still target a UDP port
// after a previous host crash (orphans keep streaming black frames onto the same RTP port).
void terminate_gst_multiudpsink_on_port(std::uint16_t port);

} // namespace archstreamer
