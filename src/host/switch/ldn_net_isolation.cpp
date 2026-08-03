#include "host/switch/ldn_net_isolation.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace archstreamer {
namespace {

constexpr const char* kBridgeName = "asldnbr0";
constexpr const char* kLibvirtNet = "archstreamer-ldn";
constexpr const char* kGateway = "172.31.200.1";
constexpr const char* kDns = "1.1.1.1";

bool file_contains(const std::filesystem::path& path, std::string_view needle) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int run_capture(const std::string& command, std::string* output = nullptr) {
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return 127;
    }
    std::array<char, 256> buffer{};
    std::string collected;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        collected += buffer.data();
    }
    const int status = pclose(pipe);
    if (output != nullptr) {
        *output = std::move(collected);
    }
    if (status == -1) {
        return 127;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 127;
}

bool command_on_path(const char* name) {
    return run_capture(std::string("command -v ") + name + " >/dev/null 2>&1") == 0;
}

std::string resolve_command_path(const char* name) {
    std::string out;
    if (run_capture(std::string("command -v ") + name + " 2>/dev/null", &out) != 0) {
        return {};
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    // Prefer the first token/line (command -v may print a function body).
    const auto space = out.find_first_of(" \t\n");
    if (space != std::string::npos) {
        out.resize(space);
    }
    return out;
}

} // namespace

bool ldn_firejail_available() {
    if (!command_on_path("firejail")) {
        return false;
    }
    // restricted-network yes → users may only use --net=none.
    const std::filesystem::path cfg{"/etc/firejail/firejail.config"};
    if (std::filesystem::is_regular_file(cfg) &&
        file_contains(cfg, "restricted-network yes")) {
        return false;
    }
    return true;
}

bool ensure_ldn_bridge() {
    if (!command_on_path("virsh")) {
        std::cerr << "LDN net: virsh not found; cannot ensure " << kLibvirtNet << " bridge\n";
        return false;
    }

    // Define once if missing.
    if (run_capture(std::string("virsh net-info ") + kLibvirtNet + " >/dev/null 2>&1") != 0) {
        const auto xml_path = std::filesystem::temp_directory_path() / "archstreamer-ldn-net.xml";
        {
            std::ofstream out(xml_path);
            out << "<network>\n"
                << "  <name>" << kLibvirtNet << "</name>\n"
                << "  <bridge name='" << kBridgeName << "' stp='off' delay='0'/>\n"
                << "  <ip address='" << kGateway << "' netmask='255.255.255.0'/>\n"
                << "</network>\n";
        }
        const auto define_cmd =
            std::string("virsh net-define ") + xml_path.string() + " >/dev/null 2>&1";
        if (run_capture(define_cmd) != 0) {
            std::cerr << "LDN net: failed to define libvirt network " << kLibvirtNet << '\n';
            return false;
        }
        (void)run_capture(std::string("virsh net-autostart ") + kLibvirtNet + " >/dev/null 2>&1");
    }

    (void)run_capture(std::string("virsh net-start ") + kLibvirtNet + " >/dev/null 2>&1");

    // Bridge may show NO-CARRIER until the first firejail veth attaches; that is OK.
    std::string link_out;
    if (run_capture(std::string("ip -br link show ") + kBridgeName + " 2>/dev/null", &link_out) !=
            0 ||
        link_out.empty()) {
        std::cerr << "LDN net: bridge " << kBridgeName << " not present after virsh net-start\n";
        return false;
    }
    return true;
}

std::string ldn_slot_ipv4(std::size_t slot_index) {
    // 172.31.200.10 + slot (slot 0 → .10, slot 1 → .11, …)
    const auto host = 10 + static_cast<unsigned>(slot_index);
    return "172.31.200." + std::to_string(host);
}

std::string ldn_guest_interface_name() {
    return "eth0";
}

std::vector<std::string> ldn_firejail_command_prefix(std::size_t slot_index) {
    if (!ldn_firejail_available()) {
        std::cerr
            << "LDN net: firejail networking unavailable "
               "(install firejail and set restricted-network no in "
               "/etc/firejail/firejail.config)\n";
        return {};
    }
    if (!ensure_ldn_bridge()) {
        return {};
    }

    auto firejail = resolve_command_path("firejail");
    if (firejail.empty()) {
        firejail = "/usr/bin/firejail";
    }
    if (access(firejail.c_str(), X_OK) != 0) {
        std::cerr << "LDN net: firejail not executable at " << firejail << '\n';
        return {};
    }

    const auto ip = ldn_slot_ipv4(slot_index);
    std::vector<std::string> prefix = {
        std::move(firejail),
        "--noprofile",
        std::string("--net=") + kBridgeName,
        std::string("--ip=") + ip,
        std::string("--defaultgw=") + kGateway,
        std::string("--dns=") + kDns,
        // AppImage / gamescope / NVIDIA need a permissive device/fs view.
        "--quiet",
    };
    std::cout
        << "LDN net: firejail slot " << slot_index << " → " << ip << " on " << kBridgeName
        << '\n';
    return prefix;
}

} // namespace archstreamer
