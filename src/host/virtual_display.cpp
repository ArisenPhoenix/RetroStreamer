#include "host/virtual_display.hpp"

#include "common/platform/process_utils.hpp"
#include "host/session_slot_lease.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
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

// Managed tools live under ~/.local/share/archstreamer even when a child profile
// has redirected XDG_DATA_HOME (Yuzu save profile, Flatpak, etc.).
std::filesystem::path archstreamer_user_data_root() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home != nullptr ? home : ""} / ".local" / "share" / "archstreamer";
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
    // Prefer $HOME/.local/share/archstreamer — never a redirected XDG_DATA_HOME.
    const auto managed = archstreamer_user_data_root() / "gamescope" / "archstreamer-gamescope";
    if (path_executable(managed)) {
        return managed.string();
    }
    // Legacy: older installs under whatever XDG_DATA_HOME pointed at install time.
    const auto legacy = xdg_data_home() / "archstreamer" / "gamescope" / "archstreamer-gamescope";
    if (legacy != managed && path_executable(legacy)) {
        return legacy.string();
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

namespace {

std::optional<std::filesystem::path> gamescope_install_root_for(
    const std::filesystem::path& gamescope_executable) {
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(gamescope_executable, ec);
    if (ec) {
        path = gamescope_executable;
    }
    if (path.filename() == "archstreamer-gamescope") {
        return path.parent_path();
    }
    // .../bin/gamescope → install root
    if (path.filename() == "gamescope" && path.parent_path().filename() == "bin") {
        return path.parent_path().parent_path();
    }
    return std::nullopt;
}

bool gamescope_executable_is_managed(const std::filesystem::path& gamescope_executable) {
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(gamescope_executable, ec);
    if (ec) {
        path = gamescope_executable;
    }
    return path.filename() == "archstreamer-gamescope";
}

std::filesystem::path find_gamescope_wsi_library(const std::filesystem::path& root) {
    for (const char* name : {
             "libVkLayer_FROG_gamescope_wsi.so",
             "libVkLayer_FROG_gamescope_wsi_x86_64.so",
         }) {
        const auto candidate = root / "lib" / "x86_64-linux-gnu" / name;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// Managed gamescope is relocated from /opt/gamescope but the WSI layer JSON still
// points at /opt/... — without a loadable layer, nested X11 only allows present on
// the "boot VGA" NVIDIA (1660 here), so Yuzu on the 3060 dies with "lacks a present queue".
void ensure_gamescope_wsi_layer_manifest(const std::filesystem::path& root) {
    const auto json_path =
        root / "share" / "vulkan" / "implicit_layer.d" / "VkLayer_FROG_gamescope_wsi.x86_64.json";
    const auto library = find_gamescope_wsi_library(root);
    if (!std::filesystem::exists(json_path) || library.empty()) {
        return;
    }

    std::ifstream in(json_path);
    if (!in) {
        return;
    }
    std::string contents(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    in.close();

    const auto key = std::string{"\"library_path\""};
    const auto key_pos = contents.find(key);
    if (key_pos == std::string::npos) {
        return;
    }
    const auto colon = contents.find(':', key_pos + key.size());
    if (colon == std::string::npos) {
        return;
    }
    const auto first_quote = contents.find('"', colon + 1);
    if (first_quote == std::string::npos) {
        return;
    }
    const auto second_quote = contents.find('"', first_quote + 1);
    if (second_quote == std::string::npos) {
        return;
    }
    const auto current = contents.substr(first_quote + 1, second_quote - first_quote - 1);
    if (std::filesystem::exists(current)) {
        return;
    }

    const auto absolute = std::filesystem::weakly_canonical(library).string();
    contents.replace(first_quote + 1, second_quote - first_quote - 1, absolute);
    std::ofstream out(json_path, std::ios::trunc);
    if (out) {
        out << contents;
    }
}

} // namespace

std::vector<std::pair<std::string, std::string>> gamescope_launch_environment() {
    // Nested clients must load Gamescope WSI so any preferred GPU can present into
    // gamescope's XWayland (otherwise dual-NVIDIA present stays stuck on one card).
    std::vector<std::pair<std::string, std::string>> environment;
    environment.emplace_back("ENABLE_GAMESCOPE_WSI", "1");

    std::filesystem::path layer_dir;
    bool using_managed = false;
    if (const auto gamescope = find_gamescope(); gamescope.has_value()) {
        if (const auto root = gamescope_install_root_for(*gamescope); root.has_value()) {
            using_managed = gamescope_executable_is_managed(*gamescope);
            ensure_gamescope_wsi_layer_manifest(*root);
            const auto candidate = *root / "share" / "vulkan" / "implicit_layer.d";
            if (std::filesystem::exists(candidate / "VkLayer_FROG_gamescope_wsi.x86_64.json") &&
                !find_gamescope_wsi_library(*root).empty()) {
                layer_dir = candidate;
            }
        }
    }
    // Never pair managed gamescope with /opt's 3.16 WSI layer — mismatched
    // layer/compositor versions segfault or skip present on the non-boot GPU.
    if (layer_dir.empty() && !using_managed &&
        std::filesystem::exists(
            "/opt/gamescope/share/vulkan/implicit_layer.d/VkLayer_FROG_gamescope_wsi.x86_64.json")) {
        layer_dir = "/opt/gamescope/share/vulkan/implicit_layer.d";
    }
    if (!layer_dir.empty()) {
        environment.emplace_back("VK_ADD_IMPLICIT_LAYER_PATH", layer_dir.string());
        // Older loaders also honor XDG_DATA_DIRS for share/vulkan/implicit_layer.d.
        const auto share = layer_dir.parent_path().parent_path(); // .../share
        std::string xdg = share.string();
        if (const char* existing = std::getenv("XDG_DATA_DIRS");
            existing != nullptr && existing[0] != '\0') {
            xdg.push_back(':');
            xdg += existing;
        } else {
            xdg += ":/usr/local/share:/usr/share";
        }
        // Keep NVIDIA ICD discoverable even if a later merge truncates dirs.
        if (xdg.find("/usr/share") == std::string::npos) {
            xdg += ":/usr/local/share:/usr/share";
        }
        environment.emplace_back("XDG_DATA_DIRS", std::move(xdg));
    }
    return environment;
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
    int expect_height,
    int owner_pid) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Prefer: (1) PID/process-group match to this session's gamescope, (2) matching
    // resolution, (3) running, (4) highest node id. Without the PID gate a stale
    // leftover gamescope can win and the encoder dies when that process exits.
    const std::string py =
        "import sys,json,os\n"
        "try:\n"
        " data=json.load(sys.stdin)\n"
        "except Exception:\n"
        " sys.exit(0)\n"
        "want_w=" + std::to_string(expect_width) + "\n"
        "want_h=" + std::to_string(expect_height) + "\n"
        "want_pid=" + std::to_string(owner_pid) + "\n"
        "def pgid(pid):\n"
        " try:\n"
        "  return os.getpgid(int(pid))\n"
        " except Exception:\n"
        "  return None\n"
        "want_pgid=pgid(want_pid) if want_pid>0 else None\n"
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
        " pid=None\n"
        " for key in ('pipewire.sec.pid','application.process.id','node.pid'):\n"
        "  if key in props:\n"
        "   try: pid=int(props[key]); break\n"
        "   except Exception: pass\n"
        " owner=0\n"
        " if want_pid>0 and pid is not None:\n"
        "  if pid==want_pid: owner=2\n"
        "  elif want_pgid is not None and pgid(pid)==want_pgid: owner=1\n"
        " state=str(info.get('state') or '')\n"
        " score=(owner, 1 if state=='running' else 0, int(oid))\n"
        " cands.append((score, oid))\n"
        "if cands:\n"
        " cands.sort()\n"
        " # If an owner_pid was requested, refuse nodes that don't match it once any\n"
        " # matching candidate exists; otherwise fall back to best remaining.\n"
        " if want_pid>0:\n"
        "  owned=[c for c in cands if c[0][0]>0]\n"
        "  if owned:\n"
        "   print(owned[-1][1]); sys.exit(0)\n"
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

namespace {

int x_display_number(const std::string& display) {
    if (display.size() < 2 || display.front() != ':') {
        return -1;
    }
    try {
        return std::stoi(display.substr(1));
    } catch (const std::exception&) {
        return -1;
    }
}

// Squatting on a live display is worse than failing: the emulator renders into
// someone else's session and their capture streams both games.
void require_free_display(const std::string& display, const char* server) {
    if (slot_lock::display_number_free(x_display_number(display))) {
        return;
    }
    throw std::runtime_error(
        std::string{server} + " cannot use " + display +
        ": an X server already owns it. Another ArchStreamer host is likely running on this "
        "machine; start this one with a different --virtual-display.");
}

void await_display_ready(const std::string& display, const ChildProcess& process) {
    const int number = x_display_number(display);
    const auto socket = "/tmp/.X11-unix/X" + std::to_string(number);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (number >= 0 && std::filesystem::exists(socket, error)) {
            return;
        }
        if (!process.running()) {
            throw std::runtime_error("X server for " + display + " exited immediately");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("X server for " + display + " did not finish starting");
}

} // namespace

XvfbDisplay::~XvfbDisplay() {
    stop();
}

void XvfbDisplay::start(const std::string& display, const std::string& resolution) {
    require_free_display(display, "Xvfb");
    display_ = display;
    process_.start({"Xvfb", display, "-screen", "0", resolution + "x24", "-nolisten", "tcp"});
    await_display_ready(display, process_);
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
    require_free_display(display, "Xephyr");
    display_ = display;
    process_.start({"Xephyr", display, "-screen", resolution, "-ac", "-noreset"});
    await_display_ready(display, process_);
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
