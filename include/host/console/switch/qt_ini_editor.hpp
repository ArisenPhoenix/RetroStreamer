#pragma once

#include <string>
#include <string_view>

namespace archstreamer {

std::string quote_qt_ini_value(std::string_view value);
void set_qt_ini_value(std::string& contents, std::string_view key, std::string_view value);
void set_qt_ini_group_value(
    std::string& contents,
    std::string_view group,
    std::string_view key,
    std::string_view value);

} // namespace archstreamer
