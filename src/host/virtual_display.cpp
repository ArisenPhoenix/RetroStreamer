#include "host/virtual_display.hpp"

#include "common/platform/process_utils.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <unistd.h>

namespace archstreamer {
namespace {

bool path_executable(const std::filesystem::path& path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
}

std::filesystem::path xdg_data_home() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg};
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home != nullptr ? home : ""} / ".local" / "share";
}

std::string normalize_pci_sysfs_name(std::string bus) {
    // nvidia-smi: 00000000:03:00.0  ->  0000:03:00.0
    if (bus.size() >= 12 && bus[8] == ':') {
        // Already domain:bus:slot.func with 8-digit domain; trim to 4.
        while (bus.size() > 12 && bus[0] == '0' && bus[1] == '0' && bus[2] == '0' && bus[3] == '0') {
            bus.erase(0, 4);
            break;
        }
        if (bus.size() > 12 && bus.find(':') == 8) {
            bus = bus.substr(4);
        }
    }
    return bus;
}

std::string shell_single_quote(const std::string& value) {
    std::string out = "'";
    for (const char character : value) {
        if (character == '\'') {
            out += "'\\''";
        } else {
            out.push_back(character);
        }
    }
    out.push_back('\'');
    return out;
}

} // namespace

VirtualDisplay::~VirtualDisplay() = default;

std::vector<std::string> VirtualDisplay::gl_command_prefix() const {
    return {};
}

std::vector<std::pair<std::string, std::string>> VirtualDisplay::gl_environment() const {
    return {};
}

bool VirtualDisplay::accelerates_opengl() const {
    return false;
}

bool VirtualDisplay::uses_pipewire_video() const {
    return false;
}

bool command_available(const char* command) {
    const auto check_path = [command](const std::filesystem::path& directory) {
        if (directory.empty()) {
            return false;
        }
        return path_executable(directory / command);
    };

    const char* path_env = std::getenv("PATH");
    if (path_env != nullptr) {
        auto paths = std::string{path_env};
        std::string::size_type start = 0;
        while (start <= paths.size()) {
            const auto end = paths.find(':', start);
            const auto directory =
                paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (check_path(directory)) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    return check_path("/usr/bin") || check_path("/usr/local/bin") ||
        check_path("/opt/VirtualGL/bin") || check_path("/opt/gamescope/bin");
}

std::optional<std::string> find_vglrun() {
    if (const char* env = std::getenv("ARCHSTREAMER_VGLRUN"); env != nullptr && *env != '\0') {
        if (path_executable(env)) {
            return std::string{env};
        }
    }
    if (path_executable("/opt/VirtualGL/bin/vglrun")) {
        return std::string{"/opt/VirtualGL/bin/vglrun"};
    }
    const auto managed = xdg_data_home() / "archstreamer" / "virtualgl" / "VirtualGL" / "bin" / "vglrun";
    if (path_executable(managed)) {
        return managed.string();
    }
    if (command_available("vglrun")) {
        return std::string{"vglrun"};
    }
    return std::nullopt;
}

std::string default_vgl_display() {
    if (const char* env = std::getenv("VGL_DISPLAY"); env != nullptr && *env != '\0') {
        return env;
    }
    return "egl";
}

std::vector<std::string> virtual_gl_command_prefix() {
    const auto vglrun = find_vglrun();
    if (!vglrun.has_value()) {
        return {};
    }
    return {*vglrun, "-sp", "+sync"};
}

std::vector<std::pair<std::string, std::string>> virtual_gl_environment() {
    return {
        {"VGL_DISPLAY", default_vgl_display()},
        {"VGL_SPOIL", "0"},
        {"VGL_SYNC", "1"},
        {"VGL_COMPRESS", "proxy"},
        {"QT_XCB_GL_INTEGRATION", "xcb_glx"},
        {"QT_OPENGL", "desktop"},
    };
}

std::optional<std::string> find_gamescope() {
    if (const char* env = std::getenv("ARCHSTREAMER_GAMESCOPE"); env != nullptr && *env != '\0') {
        if (path_executable(env)) {
            return std::string{env};
        }
    }
    // Managed 3.12 wrapper (system 3.16 from akdor PPA lacks SDL nested and is flaky headless).
    const auto managed = xdg_data_home() / "archstreamer" / "gamescope" / "archstreamer-gamescope";
    if (path_executable(managed)) {
        return managed.string();
    }
    if (path_executable("/opt/gamescope/bin/gamescope")) {
        return std::string{"/opt/gamescope/bin/gamescope"};
    }
    if (command_available("gamescope")) {
        return std::string{"gamescope"};
    }
    return std::nullopt;
}

std::vector<std::string> gamescope_command_prefix(
    int width,
    int height,
    const std::string& prefer_vk_device) {
    const auto gamescope = find_gamescope();
    if (!gamescope.has_value()) {
        return {};
    }
    std::vector<std::string> args{
        *gamescope,
        "--headless",
        "-w",
        std::to_string(width),
        "-h",
        std::to_string(height),
        "-W",
        std::to_string(width),
        "-H",
        std::to_string(height),
        "--force-windows-fullscreen",
        // -r = nested refresh. -o = refresh when unfocused (headless counts as
        // unfocused) — not "output fps". Keep them equal so 60fps games aren't capped.
        "-r",
        "60",
        "-o",
        "60",
    };
    if (!prefer_vk_device.empty()) {
        args.push_back("--prefer-vk-device");
        args.push_back(prefer_vk_device);
    }
    args.push_back("--");
    return args;
}

std::vector<std::pair<std::string, std::string>> gamescope_launch_environment() {
    // Child inherits gamescope's nested Xwayland; do not force DISPLAY=:99.
    return {};
}

std::optional<std::string> pci_vendor_device_id(const std::string& pci_bus) {
    if (pci_bus.empty()) {
        return std::nullopt;
    }
    auto name = normalize_pci_sysfs_name(pci_bus);
    const auto base = std::filesystem::path{"/sys/bus/pci/devices"} / name;
    std::ifstream vendor_in(base / "vendor");
    std::ifstream device_in(base / "device");
    if (!vendor_in || !device_in) {
        return std::nullopt;
    }
    std::string vendor;
    std::string device;
    vendor_in >> vendor;
    device_in >> device;
    if (vendor.size() < 3 || device.size() < 3) {
        return std::nullopt;
    }
    // sysfs values are like 0x10de / 0x2504
    if (vendor.rfind("0x", 0) == 0) {
        vendor = vendor.substr(2);
    }
    if (device.rfind("0x", 0) == 0) {
        device = device.substr(2);
    }
    return vendor + ":" + device;
}

std::optional<std::string> wait_for_gamescope_pipewire_node(
    std::chrono::milliseconds timeout,
    int expect_width,
    int expect_height) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Prefer matching resolution + running + highest id. Avoid latching onto a stale
    // leftover gamescope (e.g. a 640x360 probe) while the real 1280x720 session exists.
    const std::string py =
        "import sys,json\n"
        "try:\n"
        " data=json.load(sys.stdin)\n"
        "except Exception:\n"
        " sys.exit(0)\n"
        "want_w=" + std::to_string(expect_width) + "\n"
        "want_h=" + std::to_string(expect_height) + "\n"
        "cands=[]\n"
        "for o in data:\n"
        " info=o.get('info') or {}\n"
        " props=(info.get('props') or {})\n"
        " if props.get('media.name')!='gamescope' or props.get('media.class')!='Video/Source':\n"
        "  continue\n"
        " oid=o.get('id')\n"
        " if oid is None: continue\n"
        " w=h=0\n"
        " for fmt in (info.get('params') or {}).get('EnumFormat') or []:\n"
        "  size=fmt.get('size') or {}\n"
        "  if isinstance(size, dict) and 'width' in size and 'height' in size:\n"
        "   try:\n"
        "    w=int(size['width']); h=int(size['height'])\n"
        "   except Exception:\n"
        "    pass\n"
        "   break\n"
        " match=(want_w<=0 or want_h<=0) or (w==want_w and h==want_h)\n"
        " if not match: continue\n"
        " state=str(info.get('state') or '')\n"
        " score=(1 if state=='running' else 0, int(oid))\n"
        " cands.append((score, oid))\n"
        "if cands:\n"
        " cands.sort()\n"
        " print(cands[-1][1])\n";
    const std::string command = "pw-dump 2>/dev/null | python3 -c " + shell_single_quote(py);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto pw_dump = read_command_output(command.c_str());
        auto node = trim_ascii_whitespace(pw_dump);
        if (!node.empty()) {
            return node;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return std::nullopt;
}

VirtualDisplayBackend choose_virtual_display_backend(VirtualDisplayBackend requested) {
    if (requested == VirtualDisplayBackend::Gamescope) {
        if (!find_gamescope().has_value()) {
            throw std::runtime_error(
                "gamescope not found. Install gamescope or set ARCHSTREAMER_GAMESCOPE "
                "(managed copy: ~/.local/share/archstreamer/gamescope/archstreamer-gamescope)");
        }
        return VirtualDisplayBackend::Gamescope;
    }
    if (requested == VirtualDisplayBackend::VirtualGL) {
        if (!command_available("Xvfb")) {
            throw std::runtime_error("VirtualGL capture requires Xvfb; install xvfb");
        }
        if (!find_vglrun().has_value()) {
            throw std::runtime_error(
                "VirtualGL not found (expected vglrun or /opt/VirtualGL/bin/vglrun). "
                "Install VirtualGL or set ARCHSTREAMER_VGLRUN");
        }
        return VirtualDisplayBackend::VirtualGL;
    }
    if (requested != VirtualDisplayBackend::None) {
        return requested;
    }
    if (command_available("Xvfb")) {
        return VirtualDisplayBackend::Xvfb;
    }
    if (command_available("Xephyr")) {
        return VirtualDisplayBackend::Xephyr;
    }

    throw std::runtime_error("no virtual display backend found; install Xvfb or Xephyr");
}

std::unique_ptr<VirtualDisplay> make_virtual_display(VirtualDisplayBackend backend) {
    switch (choose_virtual_display_backend(backend)) {
    case VirtualDisplayBackend::Gamescope:
        return std::make_unique<GamescopeDisplay>();
    case VirtualDisplayBackend::VirtualGL:
        return std::make_unique<VirtualGlDisplay>();
    case VirtualDisplayBackend::Xephyr:
        return std::make_unique<XephyrDisplay>();
    case VirtualDisplayBackend::Xvfb:
        return std::make_unique<XvfbDisplay>();
    case VirtualDisplayBackend::None:
        return nullptr;
    }
    return nullptr;
}

XvfbDisplay::~XvfbDisplay() {
    stop();
}

void XvfbDisplay::start(const std::string& display, const std::string& resolution) {
    display_ = display;
    process_.start({"Xvfb", display, "-screen", "0", resolution + "x24", "-nolisten", "tcp"});
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

void XvfbDisplay::stop() {
    process_.stop();
}

std::string XvfbDisplay::display() const {
    return display_;
}

VirtualDisplayBackend XvfbDisplay::backend() const {
    return VirtualDisplayBackend::Xvfb;
}

XephyrDisplay::~XephyrDisplay() {
    stop();
}

void XephyrDisplay::start(const std::string& display, const std::string& resolution) {
    display_ = display;
    process_.start({"Xephyr", display, "-screen", resolution, "-ac", "-noreset"});
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
}

void XephyrDisplay::stop() {
    process_.stop();
}

std::string XephyrDisplay::display() const {
    return display_;
}

VirtualDisplayBackend XephyrDisplay::backend() const {
    return VirtualDisplayBackend::Xephyr;
}

VirtualDisplayBackend VirtualGlDisplay::backend() const {
    return VirtualDisplayBackend::VirtualGL;
}

std::vector<std::string> VirtualGlDisplay::gl_command_prefix() const {
    auto prefix = virtual_gl_command_prefix();
    if (prefix.empty()) {
        throw std::runtime_error(
            "VirtualGL required but vglrun was not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
    }
    return prefix;
}

std::vector<std::pair<std::string, std::string>> VirtualGlDisplay::gl_environment() const {
    return virtual_gl_environment();
}

bool VirtualGlDisplay::accelerates_opengl() const {
    return true;
}

void GamescopeDisplay::start(const std::string& display, const std::string& resolution) {
    display_ = display;
    const auto x = resolution.find('x');
    if (x != std::string::npos) {
        try {
            width_ = std::stoi(resolution.substr(0, x));
            height_ = std::stoi(resolution.substr(x + 1));
        } catch (const std::exception&) {
            width_ = 1920;
            height_ = 1080;
        }
    }
    if (!find_gamescope().has_value()) {
        throw std::runtime_error("gamescope not found");
    }
    // No X server process — gamescope --headless owns nested Xwayland + PipeWire output.
}

void GamescopeDisplay::stop() {}

std::string GamescopeDisplay::display() const {
    return display_;
}

VirtualDisplayBackend GamescopeDisplay::backend() const {
    return VirtualDisplayBackend::Gamescope;
}

std::vector<std::string> GamescopeDisplay::gl_command_prefix() const {
    auto prefix = gamescope_command_prefix(width_, height_, prefer_vk_device_);
    if (prefix.empty()) {
        throw std::runtime_error("gamescope required but was not found; set ARCHSTREAMER_GAMESCOPE");
    }
    return prefix;
}

std::vector<std::pair<std::string, std::string>> GamescopeDisplay::gl_environment() const {
    return gamescope_launch_environment();
}

bool GamescopeDisplay::uses_pipewire_video() const {
    return true;
}

void GamescopeDisplay::set_prefer_vk_device(std::string vendor_device) {
    prefer_vk_device_ = std::move(vendor_device);
}

void GamescopeDisplay::set_nested_size(int width, int height) {
    if (width > 0) {
        width_ = width;
    }
    if (height > 0) {
        height_ = height;
    }
}

} // namespace archstreamer
