#include "host/capture_platform.hpp"

#include "common/platform/process_utils.hpp"
#include "host/gstreamer_media_server.hpp"
#include "host/gpu_select.hpp"
#include "host/retroarch_config_writer.hpp"
#include "host/virtual_display.hpp"

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

CapturePlan resolve_capture_plan(
    HostAppConfig& config,
    const RetroArchLaunchConfig& launch_config) {
    CapturePlan plan;
    plan.use_virtual_capture = config.video;
    plan.capture_fullscreen = config.video;
    plan.capture_display = config.virtual_display;
    plan.display_backend = config.display_backend;

    if (launch_config.standalone && plan.use_virtual_capture &&
        plan.display_backend == VirtualDisplayBackend::None) {
        plan.display_backend = VirtualDisplayBackend::Gamescope;
    }
    if (!launch_config.standalone && plan.use_virtual_capture &&
        (plan.display_backend == VirtualDisplayBackend::None ||
         plan.display_backend == VirtualDisplayBackend::Xvfb) &&
        core_needs_gl_on_virtual_display(launch_config.core_path) &&
        find_vglrun().has_value() && command_available("Xvfb")) {
        // Only wrap HW GL cores. Software cores use plain Xvfb + sdl2.
        plan.display_backend = VirtualDisplayBackend::VirtualGL;
    } else if (!launch_config.standalone && plan.use_virtual_capture &&
               plan.display_backend == VirtualDisplayBackend::VirtualGL &&
               !core_needs_gl_on_virtual_display(launch_config.core_path)) {
        std::cout << "Display: plain Xvfb for software core (skipping VirtualGL)\n";
        plan.display_backend = VirtualDisplayBackend::Xvfb;
    }

    // Software cores: keep the virtual framebuffer modest for Wi‑Fi clients.
    if (!launch_config.standalone && plan.use_virtual_capture &&
        !core_needs_gl_on_virtual_display(launch_config.core_path)) {
        int width = 0;
        int height = 0;
        parse_resolution(config.video_resolution, width, height);
        if (width > 1280 || height > 720) {
            std::cout
                << "Capture resolution capped to 1280x720 for software core "
                << "(was " << config.video_resolution << ")\n";
            config.video_resolution = "1280x720";
        }
    }

    plan.gamescope_capture =
        plan.use_virtual_capture && plan.display_backend == VirtualDisplayBackend::Gamescope;
    plan.virtualgl_capture =
        plan.use_virtual_capture && plan.display_backend == VirtualDisplayBackend::VirtualGL;
    return plan;
}

void normalize_audio_backend_for_platform(HostAppConfig& /*config*/) {
    // Pulse/PipeWire are valid on Linux.
}

void apply_standalone_capture_prefix(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const HostAppConfig& config,
    const std::string& gamescope_vk_device) {
    if (capture.gamescope_capture) {
        int width = 1280;
        int height = 720;
        parse_resolution(config.video_resolution, width, height);
        auto prefix = gamescope_command_prefix(width, height, gamescope_vk_device);
        if (prefix.empty()) {
            throw std::runtime_error(
                "Switch streaming requires gamescope. Install it or set ARCHSTREAMER_GAMESCOPE "
                "(managed: ~/.local/share/archstreamer/gamescope/archstreamer-gamescope)");
        }
        launch_config.command_prefix = std::move(prefix);
        std::cout
            << "Yuzu: gamescope headless via " << launch_config.command_prefix.front()
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

    if (capture.virtualgl_capture) {
        auto prefix = virtual_gl_command_prefix();
        if (prefix.empty()) {
            throw std::runtime_error(
                "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
        }
        launch_config.command_prefix = std::move(prefix);
        std::cout
            << "Yuzu: VirtualGL OpenGL via " << launch_config.command_prefix.front()
            << " (3D " << default_vgl_display()
            << ", capture " << capture.capture_display << ")\n";
    }
}

void apply_retroarch_vgl_prefix(
    RetroArchLaunchConfig& launch_config,
    const CapturePlan& capture,
    const std::optional<GpuDevice>& resolved_gpu) {
    if (!capture.virtualgl_capture) {
        return;
    }
    auto prefix = virtual_gl_command_prefix();
    if (prefix.empty()) {
        throw std::runtime_error(
            "VirtualGL (vglrun) not found; install VirtualGL or set ARCHSTREAMER_VGLRUN");
    }
    prefix.insert(
        prefix.end(),
        launch_config.command_prefix.begin(),
        launch_config.command_prefix.end());
    launch_config.command_prefix = std::move(prefix);
    std::cout
        << "RetroArch: VirtualGL via " << launch_config.command_prefix.front()
        << " (3D on " << default_vgl_display()
        << ", capture " << capture.capture_display;
    if (resolved_gpu.has_value() && !resolved_gpu->prime_provider.empty()) {
        std::cout << ", PRIME " << resolved_gpu->prime_provider;
    }
    std::cout << ")\n";
}

void start_deferred_gamescope_video_if_needed(
    MediaServer* media_server,
    const HostAppConfig& config,
    std::vector<MediaClientStream>& media_streams) {
    auto* gst = dynamic_cast<GStreamerMediaServer*>(media_server);
    if (gst == nullptr || !gst->video_deferred()) {
        return;
    }
    if (config.verbose) {
        std::cout << "Waiting for gamescope PipeWire video node...\n";
    }
    int expect_w = 1280;
    int expect_h = 720;
    parse_resolution(config.video_resolution, expect_w, expect_h);
    const auto node = wait_for_gamescope_pipewire_node(
        std::chrono::seconds(20),
        expect_w,
        expect_h);
    if (!node.has_value()) {
        throw std::runtime_error(
            "gamescope did not publish a PipeWire Video/Source (media.name=gamescope)");
    }
    if (config.verbose) {
        std::cout << "Gamescope PipeWire node: " << *node << '\n';
    }
    gst->start_pipewire_video(*node, media_streams);
}

} // namespace archstreamer
