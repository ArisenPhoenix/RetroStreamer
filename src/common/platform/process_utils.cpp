#include "common/platform/process_utils.hpp"

#include <array>
#include <cstdio>

#ifdef _WIN32
#define archstreamer_popen _popen
#define archstreamer_pclose _pclose
#else
#define archstreamer_popen popen
#define archstreamer_pclose pclose
#endif

namespace archstreamer {

std::string trim_ascii_whitespace(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == '\n' || value[start] == '\r' || value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

std::string read_command_output(const char* command) {
    auto* pipe = archstreamer_popen(command, "r");
    if (pipe == nullptr) {
        return {};
    }

    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    archstreamer_pclose(pipe);
    return trim_ascii_whitespace(std::move(output));
}

} // namespace archstreamer
