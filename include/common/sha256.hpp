#pragma once

#include <string>
#include <string_view>

namespace archstreamer {

std::string sha256_hex(std::string_view input);

} // namespace archstreamer
