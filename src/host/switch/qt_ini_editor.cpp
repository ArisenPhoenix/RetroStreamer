#include "host/switch/qt_ini_editor.hpp"

namespace archstreamer {

std::string quote_qt_ini_value(std::string_view value) {
    if (value == "[empty]") {
        return "[empty]";
    }
    const bool needs_quotes = value.find_first_of(",;\"\n\r") != std::string_view::npos;
    if (!needs_quotes) {
        return std::string(value);
    }
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    quoted.append(value);
    quoted.push_back('"');
    return quoted;
}

void set_qt_ini_value(std::string& contents, std::string_view key, std::string_view value) {
    const std::string prefix = std::string(key) + "=";
    const std::string line = prefix + quote_qt_ini_value(value);

    std::size_t pos = 0;
    while (true) {
        pos = contents.find(prefix, pos);
        if (pos == std::string::npos) {
            if (!contents.empty() && contents.back() != '\n') {
                contents.push_back('\n');
            }
            contents += line;
            contents.push_back('\n');
            return;
        }
        if (pos == 0 || contents[pos - 1] == '\n') {
            break;
        }
        pos += prefix.size();
    }

    const auto end = contents.find('\n', pos);
    if (end == std::string::npos) {
        contents.replace(pos, contents.size() - pos, line);
    } else {
        contents.replace(pos, end - pos, line);
    }
}

void set_qt_ini_group_value(
    std::string& contents,
    std::string_view group,
    std::string_view key,
    std::string_view value) {
    const std::string section_header = "[" + std::string(group) + "]";
    auto section_pos = contents.find(section_header);
    if (section_pos == std::string::npos) {
        if (!contents.empty() && contents.back() != '\n') {
            contents.push_back('\n');
        }
        contents += '\n';
        contents += section_header;
        contents += '\n';
        contents += std::string(key);
        contents += '=';
        contents += quote_qt_ini_value(value);
        contents += '\n';
        return;
    }

    auto body_start = section_pos + section_header.size();
    if (body_start < contents.size() && contents[body_start] == '\r') {
        ++body_start;
    }
    if (body_start < contents.size() && contents[body_start] == '\n') {
        ++body_start;
    }

    const auto next_section = contents.find("\n[", body_start);
    const auto body_end = next_section == std::string::npos ? contents.size() : next_section;
    std::string body = contents.substr(body_start, body_end - body_start);
    set_qt_ini_value(body, key, value);
    contents.replace(body_start, body_end - body_start, body);
}

} // namespace archstreamer
