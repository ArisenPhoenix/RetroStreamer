#include "host/video/capture_platform.hpp"

#include "common/platform/process_utils.hpp"
#include "host/video/gstreamer_media_server.hpp"
#include "host/video/gamescope_pipewire_node.hpp"
#include "host/video/virtual_display.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace archstreamer {
namespace {

void parse_resolution(const std::string& value, int& width, int& height) {
    const auto x_pos = value.find('x');
    if (x_pos == std::string::npos) {
        return;
    }
    try {
        width = std::stoi(value.substr(0, x_pos));
        height = std::stoi(value.substr(x_pos + 1));
    } catch (const std::exception&) {
    }
}

} // namespace

bool platform_supports_gamescope_capture() {
    return true;
}

bool platform_supports_virtualgl_capture() {
    return true;
}

PresentTarget resolve_present_target(CaptureResolveInput& input) {
    PresentTarget present;
    present.display = input.capture_display;
    if (!input.stream_video) {
        return present;
    }
    present.backend = input.preferred_backend;

    if (input.standalone && present.backend == VirtualDisplayBackend::None) {
        present.backend = VirtualDisplayBackend::Gamescope;
    }
    if (!input.standalone &&
        (present.backend == VirtualDisplayBackend::None ||
         present.backend == VirtualDisplayBackend::Xvfb) &&
        input.core_needs_gl &&
        find_vglrun().has_value() && command_available("Xvfb")) {
        // Only wrap HW GL cores. Software cores use plain Xvfb + sdl2.
        present.backend = VirtualDisplayBackend::VirtualGL;
    } else if (!input.standalone &&
               present.backend == VirtualDisplayBackend::VirtualGL &&
               !input.core_needs_gl) {
        std::cout << "Display: plain Xvfb for software core (skipping VirtualGL)\n";
        present.backend = VirtualDisplayBackend::Xvfb;
    }

    // Software cores: keep the virtual framebuffer modest for Wi‑Fi clients.
    if (!input.standalone && !input.core_needs_gl) {
        int width = 0;
        int height = 0;
        parse_resolution(input.video_resolution, width, height);
        if (width > 1280 || height > 720) {
            std::cout
                << "Capture resolution capped to 1280x720 for software core "
                << "(was " << input.video_resolution << ")\n";
            input.video_resolution = "1280x720";
        }
    }

    if (present.backend == VirtualDisplayBackend::None) {
        present.backend = VirtualDisplayBackend::Xvfb;
    }
    return present;
}

void apply_standalone_capture_prefix(
    std::vector<std::string>& command_prefix,
    const PresentTarget& present,
    const std::string& video_resolution,
    const std::string& gamescope_vk_device) {
    if (present.uses_pipewire()) {
        int width = 1280;
        int height = 720;
        parse_resolution(video_resolution, width, height);
        auto prefix = gamescope_command_prefix(width, height, gamescope_vk_device);
        if (prefix.empty()) {
            throw std::runtime_error(
                "Switch streaming requires gamescope. Install it or set ARCHSTREAMER_GAMESCOPE "
                "(managed: ~/.local/share/archstreamer/gamescope/archstreamer-gamescope)");
        }
        command_prefix = std::move(prefix);
        std::cout
            << "Yuzu: gamescope headless via " << command_prefix.front()
            << " (" << width << "x" << height
            << ", prefer-vk-device " << gamescope_vk_device
            << ", Gamescope WSI enabled)\n";
        for (const auto& [key, value] : gamescope_launch_environment()) {
            if (key == "VK_ADD_IMPLICIT_LAYER_PATH") {
                std::cout << "Gamescope WSI layer path: " << value << '\n';
                break;
            }
        }
        return;
    }

    if (present.uses_virtualgl()) {
        auto prefix = virtual_gl_command_prefix();
        if (prefix.empty()) {
            throw std::runtime_error(
                "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
        }
        command_prefix = std::move(prefix);
        std::cout
            << "Yuzu: VirtualGL OpenGL via " << command_prefix.front()
            << " (3D " << default_vgl_display()
            << ", capture " << present.display << ")\n";
    }
}

void apply_retroarch_vgl_prefix(
    std::vector<std::string>& command_prefix,
    const PresentTarget& present,
    const std::optional<GpuTarget>& resolved_gpu) {
    if (!present.uses_virtualgl()) {
        return;
    }
    auto prefix = virtual_gl_command_prefix();
    if (prefix.empty()) {
        throw std::runtime_error(
            "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
    }
    prefix.insert(prefix.end(), command_prefix.begin(), command_prefix.end());
    command_prefix = std::move(prefix);
    std::cout
        << "RetroArch: VirtualGL via " << command_prefix.front()
        << " (3D on " << default_vgl_display()
        << ", capture " << present.display;
    if (resolved_gpu.has_value() && !resolved_gpu->prime_provider.empty()) {
        std::cout << ", PRIME " << resolved_gpu->prime_provider;
    }
    std::cout << ")\n";
}

void start_deferred_gamescope_video_if_needed(
    MediaServer* media_server,
    const std::string& video_resolution,
    std::vector<MediaClientStream>& media_streams,
    int owner_pid) {
    auto* gst = dynamic_cast<GStreamerMediaServer*>(media_server);
    if (gst == nullptr || !gst->video_deferred()) {
        return;
    }
    std::cout
        << "Waiting for gamescope PipeWire video node"
        << (owner_pid > 0 ? (" (owner pid " + std::to_string(owner_pid) + ")") : "")
        << "...\n";
    int expect_w = 1280;
    int expect_h = 720;
    parse_resolution(video_resolution, expect_w, expect_h);
    const auto node = wait_for_gamescope_pipewire_node(
        std::chrono::seconds(20),
        expect_w,
        expect_h,
        owner_pid);
    if (!node.has_value()) {
        throw std::runtime_error(
            "gamescope did not publish a PipeWire Video/Source for this session "
            "(media.name=gamescope; timed out waiting for this slot's node)");
    }
    std::cout << "Gamescope PipeWire target: path=" << node->path;
    if (!node->target_object.empty()) {
        std::cout << " target-object=" << node->target_object;
    }
    std::cout << '\n';
    gst->start_pipewire_video(*node, media_streams);
}

} // namespace archstreamer
