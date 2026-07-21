#pragma once

#include <string>

namespace archstreamer {

std::string trim_ascii_whitespace(std::string value);
std::string read_command_output(const char* command);

} // namespace archstreamer
