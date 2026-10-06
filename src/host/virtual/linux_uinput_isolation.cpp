#ifndef _WIN32
#include "host/virtual/linux_uinput_isolation.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef UI_GET_SYSNAME
#define UI_GET_SYSNAME(len) _IOC(_IOC_READ, 'U', 44, len)
#endif

namespace archstreamer {
namespace {

std::mutex g_mu;
std::vector<IsolatedPadNode> g_nodes;

std::string sibling_of_self(const std::string& name) {
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return name;
    }
    self[n] = '\0';
    return (std::filesystem::path(self).parent_path() / name).string();
}

std::optional<std::string> sysname_for_uinput(int uinput_fd) {
    char sysname[64]{};
    if (ioctl(uinput_fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
        return std::nullopt;
    }
    if (sysname[0] == '\0') {
        return std::nullopt;
    }
    return std::string(sysname);
}

std::vector<std::string> nodes_under_sysfs_input(const std::filesystem::path& input_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(input_dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(input_dir, ec)) {
        if (ec) {
            break;
        }
        const auto name = entry.path().filename().string();
        if (name.rfind("event", 0) != 0 && name.rfind("js", 0) != 0) {
            continue;
        }
        const auto dev = std::filesystem::path("/dev/input") / name;
        if (std::filesystem::exists(dev, ec)) {
            out.push_back(dev.string());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

void register_isolated_pad_node(IsolatedPadNode node) {
    if (node.path.empty() || node.fd < 0) {
        return;
    }
    std::lock_guard lock(g_mu);
    for (auto& existing : g_nodes) {
        if (existing.path == node.path) {
            if (existing.fd >= 0 && existing.fd != node.fd) {
                close(existing.fd);
            }
            existing = std::move(node);
            return;
        }
    }
    g_nodes.push_back(std::move(node));
}

void clear_isolated_pad_nodes_for_paths(const std::vector<std::string>& paths) {
    std::lock_guard lock(g_mu);
    g_nodes.erase(
        std::remove_if(
            g_nodes.begin(),
            g_nodes.end(),
            [&](IsolatedPadNode& node) {
                if (std::find(paths.begin(), paths.end(), node.path) == paths.end()) {
                    return false;
                }
                if (node.fd >= 0) {
                    close(node.fd);
                }
                return true;
            }),
        g_nodes.end());
}

std::vector<IsolatedPadNode> isolated_pad_nodes() {
    std::lock_guard lock(g_mu);
    return g_nodes;
}

std::string pad_access_library_path() {
    const auto sibling = sibling_of_self("libarchstreamer_pad_access.so");
    std::error_code ec;
    if (std::filesystem::exists(sibling, ec)) {
        return sibling;
    }
    return "libarchstreamer_pad_access.so";
}

std::string existing_env_value(const ProcessEnvironment& env, const char* key) {
    for (const auto& [name, value] : env.entries) {
        if (name == key) {
            return value;
        }
    }
    if (const char* from_host = getenv(key); from_host != nullptr && *from_host != '\0') {
        return from_host;
    }
    return {};
}

void contribute_isolated_pad_environment(ProcessEnvironment& env) {
    const auto nodes = isolated_pad_nodes();
    if (nodes.empty()) {
        return;
    }
    std::ostringstream spec;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i != 0) {
            spec << ':';
        }
        spec << nodes[i].path << '=' << nodes[i].fd;
    }
    env.set("ARCHSTREAMER_PAD_ACCESS", spec.str());
    const auto so = pad_access_library_path();
    env.set("ARCHSTREAMER_PAD_ACCESS_SO", so);
    std::string preload = so;
    if (const auto existing = existing_env_value(env, "LD_PRELOAD");
        !existing.empty() && existing.find(so) == std::string::npos) {
        preload.push_back(':');
        preload += existing;
    }
    env.set("LD_PRELOAD", std::move(preload));
}

void apply_isolated_pads_in_child() {
    const char* spec = getenv("ARCHSTREAMER_PAD_ACCESS");
    if (spec == nullptr || *spec == '\0') {
        int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
        if (max_fd < 1024) {
            max_fd = 1024;
        }
        if (max_fd > 65536) {
            max_fd = 65536;
        }
        for (int fd = 3; fd < max_fd; ++fd) {
            close(fd);
        }
        return;
    }

    struct Keep {
        std::string path;
        int fd = -1;
    };
    std::vector<Keep> keep;
    {
        std::string token;
        std::istringstream in(spec);
        while (std::getline(in, token, ':')) {
            const auto eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            Keep item;
            item.path = token.substr(0, eq);
            item.fd = atoi(token.c_str() + eq + 1);
            if (!item.path.empty() && item.fd >= 3) {
                keep.push_back(std::move(item));
            }
        }
    }

    int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
    if (max_fd < 1024) {
        max_fd = 1024;
    }
    if (max_fd > 65536) {
        max_fd = 65536;
    }
    for (int fd = 3; fd < max_fd; ++fd) {
        const bool keep_fd = std::any_of(keep.begin(), keep.end(), [fd](const Keep& item) {
            return item.fd == fd;
        });
        if (!keep_fd) {
            close(fd);
        }
    }

    std::ostringstream rewritten;
    int next = 3;
    for (auto& item : keep) {
        if (item.fd != next) {
            const int copied = dup2(item.fd, next);
            if (copied != next) {
                continue;
            }
            if (item.fd != next) {
                close(item.fd);
            }
            item.fd = next;
        }
        (void)fcntl(item.fd, F_SETFD, 0);
        if (next > 3) {
            rewritten << ':';
        }
        rewritten << item.path << '=' << item.fd;
        ++next;
    }
    setenv("ARCHSTREAMER_PAD_ACCESS", rewritten.str().c_str(), 1);
    if (const char* so = getenv("ARCHSTREAMER_PAD_ACCESS_SO"); so != nullptr && *so != '\0') {
        const char* existing = getenv("LD_PRELOAD");
        if (existing == nullptr || std::string(existing).find(so) == std::string::npos) {
            std::string preload = so;
            if (existing != nullptr && *existing != '\0') {
                preload.push_back(':');
                preload += existing;
            }
            setenv("LD_PRELOAD", preload.c_str(), 1);
        }
    }
}

int hold_input_node_for_session(const std::string& path) {
    if (path.empty()) {
        return -1;
    }
    int fd = -1;
    for (int attempt = 0; attempt < 50; ++attempt) {
        fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (fd < 0) {
        std::cerr
            << "Virtual pad: could not hold " << path << ": "
            << std::strerror(errno) << '\n';
        return -1;
    }
    (void)fcntl(fd, F_SETFD, 0);
    if (path.find("/js") != std::string::npos) {
        return fd;
    }
    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
        std::cout
            << "Virtual pad grabbed (other processes keep the node but get no events): "
            << path << '\n';
        return fd;
    }
    if (errno != ENOTTY) {
        std::cerr
            << "Virtual pad: EVIOCGRAB " << path << " failed: "
            << std::strerror(errno) << '\n';
    }
    return fd;
}

std::vector<std::string> input_dev_nodes_for_uinput(int uinput_fd) {
    if (uinput_fd < 0) {
        return {};
    }
    if (const auto sysname = sysname_for_uinput(uinput_fd)) {
        const auto from_sys = nodes_under_sysfs_input(
            std::filesystem::path("/sys/devices/virtual/input") / *sysname);
        if (!from_sys.empty()) {
            return from_sys;
        }
        const auto from_class = nodes_under_sysfs_input(
            std::filesystem::path("/sys/class/input") / *sysname);
        if (!from_class.empty()) {
            return from_class;
        }
    }
    return {};
}

bool hide_input_node_from_desktop(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (chmod(path.c_str(), 0) == 0) {
        std::cout << "Virtual pad hidden from desktop: " << path << '\n';
        return true;
    }
    return false;
}

} // namespace archstreamer
#endif
