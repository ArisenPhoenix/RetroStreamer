#include "common/platform/process_utils.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

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

void terminate_gst_multiudpsink_on_port(std::uint16_t port) {
#ifndef _WIN32
    if (port == 0) {
        return;
    }
    const auto port_token = ":" + std::to_string(port);
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
        std::string cmdline((std::istreambuf_iterator<char>(cmdline_file)),
                            std::istreambuf_iterator<char>());
        for (char& c : cmdline) {
            if (c == '\0') {
                c = ' ';
            }
        }
        if (cmdline.find("gst-launch") == std::string::npos ||
            cmdline.find("multiudpsink") == std::string::npos ||
            cmdline.find(port_token) == std::string::npos) {
            continue;
        }
        kill(pid, SIGTERM);
    }
#else
    (void)port;
#endif
}

} // namespace archstreamer
