#include "host/switch/switch_backend.hpp"

#include "host/switch_save_share.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string_view>

namespace archstreamer {
namespace {

bool name_looks_like_ryujinx(std::string_view name) {
    std::string lower(name);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower.find("ryujinx") != std::string::npos;
}

} // namespace

std::vector<std::string> SwitchBackend::post_exit_sync(const SaveProfile& profile) const {
    return sync_switch_shared_saves_for_profile(profile);
}

void SwitchBackend::apply_common_prep(
    RetroArchLaunchConfig& launch_config,
    SwitchBackendPrepContext& ctx,
    SwitchBackendPrepResult& result) const {
    result.resolved_pads = std::move(ctx.resolved_pads);

    if (result.resolved_pads.empty()) {
        result.resolved_pads = find_archstreamer_sdl_pads(
            ctx.players,
            ctx.ignore_controller,
            ctx.verbose,
            ctx.product_id_base);
    }

    if (ctx.graphics_api == GraphicsApiPreference::OpenGL) {
        result.force_opengl = true;
    } else if (ctx.graphics_api == GraphicsApiPreference::Vulkan) {
        if (ctx.virtualgl_capture) {
            std::cerr << "Warning: VirtualGL path cannot present Vulkan; using OpenGL.\n";
            result.force_opengl = true;
        } else {
            result.force_vulkan = true;
        }
    } else if (ctx.virtualgl_capture) {
        result.force_opengl = true;
    } else if (ctx.gamescope_capture) {
        result.force_vulkan = true;
    }

    launch_config.quiet_stdio = !ctx.verbose;
}

void SwitchBackend::finish_prep_save_sync(
    const SwitchBackendPrepContext& ctx,
    SwitchBackendPrepResult& result) const {
    const auto synced = sync_switch_shared_saves_for_profile(ctx.save_profile);
    result.synced_title_count = synced.size();
}

std::unique_ptr<SwitchBackend> make_switch_backend(
    const ResolvedStandaloneEmulator& runtime) {
    if (name_looks_like_ryujinx(runtime.display_name) ||
        name_looks_like_ryujinx(runtime.path.filename().string())) {
        return std::make_unique<RyujinxBackend>();
    }
    return std::make_unique<YuzuBackend>();
}

void log_switch_backend_prep(
    const SwitchBackend& backend,
    const EmulatorLaunchEnvRequest& env,
    const SwitchBackendPrepResult& prep,
    int resolution_scale,
    const std::optional<GpuDevice>& resolved_gpu,
    std::optional<int> slot_index) {
    (void)backend;
    if (slot_index.has_value()) {
        if (env.ryujinx_profile.has_value()) {
            std::cout
                << "session slot " << *slot_index << ": Ryujinx (ldn_mitm)"
                << " config=" << env.ryujinx_profile->data_root
                << " shared_saves=" << prep.synced_title_count << '\n';
        } else if (prep.synced_title_count > 0) {
            std::cout
                << "session slot " << *slot_index << ": Yuzu fallback; synced "
                << prep.synced_title_count << " Switch save title(s)\n";
        }
        return;
    }

    if (env.ryujinx_profile.has_value()) {
        const auto& ryujinx_user = *env.ryujinx_profile;
        std::cout
            << "Ryujinx (ldn_mitm) config: " << ryujinx_user.data_root
            << "\nRyujinx keys:            " << ryujinx_user.keys_directory
            << "\nShared Switch saves:     " << prep.synced_title_count
            << " title(s)\n";
        const int scale = std::clamp(resolution_scale, 1, 4);
        std::cout << "Ryujinx resolution: " << scale << "x native\n";
        return;
    }

    if (!env.yuzu_profile.has_value()) {
        return;
    }
    const auto& yuzu_user = *env.yuzu_profile;
    std::cout
        << "Yuzu renderer: "
        << (prep.force_opengl ? "OpenGL"
            : prep.force_vulkan ? "Vulkan" : "default");
    if (prep.yuzu_vulkan_device >= 0 && resolved_gpu.has_value()) {
        std::cout
            << " (vulkan_device=" << prep.yuzu_vulkan_device
            << " → " << resolved_gpu->name << ")";
    }
    std::cout << '\n';
    {
        const int scale = std::clamp(resolution_scale, 1, 6);
        std::cout << "Switch resolution: " << scale << "x native"
                  << " (resolution_setup=" << (scale + 1) << ")\n";
    }
    std::cout
        << "Yuzu user data: " << yuzu_user.xdg_data_home
        << "\nYuzu keys:      " << yuzu_user.keys_directory << '\n';
}

} // namespace archstreamer
