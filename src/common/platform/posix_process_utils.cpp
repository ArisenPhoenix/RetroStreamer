#include "common/platform/process_utils.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <signal.h>
#include <unistd.h>

namespace archstreamer {
namespace {

// Only a clients=host:port entry counts. A bare ":5027" search also matches
// ximagesrc display-name=:99 style arguments, which would kill another
// session's capture.
bool multiudpsink_targets_port(const std::string& cmdline, const std::string& port) {
    std::size_t position = 0;
    while ((position = cmdline.find("clients=", position)) != std::string::npos) {
        position += 8;
        const auto argument_end = cmdline.find(' ', position);
        const auto list = cmdline.substr(
            position,
            argument_end == std::string::npos ? std::string::npos : argument_end - position);
        std::size_t entry_start = 0;
        while (entry_start <= list.size()) {
            const auto comma = list.find(',', entry_start);
            const auto entry = list.substr(
                entry_start,
                comma == std::string::npos ? std::string::npos : comma - entry_start);
            if (const auto colon = entry.rfind(':');
                colon != std::string::npos && entry.substr(colon + 1) == port) {
                return true;
            }
            if (comma == std::string::npos) {
                break;
            }
            entry_start = comma + 1;
        }
        if (argument_end == std::string::npos) {
            break;
        }
        position = argument_end;
    }
    return false;
}

} // namespace

std::string trim_ascii_whitespace(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() &&
           (value[start] == '\n' || value[start] == '\r' || value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

std::string read_command_output(const char* command) {
    auto* pipe = popen(command, "r");
    if (pipe == nullptr) {
        return {};
    }

    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return trim_ascii_whitespace(std::move(output));
}

int run_command_exit_code(const char* command) {
    if (command == nullptr || command[0] == '\0') {
        return -1;
    }
    return std::system(command);
}

void terminate_gst_multiudpsink_on_port(std::uint16_t port) {
    if (port == 0) {
        return;
    }
    const auto port_token = std::to_string(port);
    const auto self = getpid();
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec || !entry.is_directory(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        int pid = 0;
        try {
            pid = std::stoi(name);
        } catch (...) {
            continue;
        }
        if (pid <= 1 || pid == self) {
            continue;
        }
        std::ifstream cmdline_file(entry.path() / "cmdline", std::ios::binary);
        if (!cmdline_file) {
            continue;
        }
        std::string cmdline(
            (std::istreambuf_iterator<char>(cmdline_file)), std::istreambuf_iterator<char>());
        for (char& c : cmdline) {
            if (c == '\0') {
                c = ' ';
            }
        }
        if (cmdline.find("gst-launch") == std::string::npos ||
            cmdline.find("multiudpsink") == std::string::npos ||
            !multiudpsink_targets_port(cmdline, port_token)) {
            continue;
        }
        kill(pid, SIGTERM);
    }
}

} // namespace archstreamer
