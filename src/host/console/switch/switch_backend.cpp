#include "host/console/switch/switch_backend.hpp"

#include "host/virtual/pad_plan.hpp"
#include "host/console/switch/ryujinx_game_cache.hpp"
#include "host/console/switch_save_share.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
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

std::vector<std::string> SwitchBackend::post_exit_sync(
    const SaveProfile& profile,
    std::string_view content_stem,
    std::string_view title_id,
    bool uses_m3m_map) const {
    if (!content_stem.empty()) {
        const auto leaf =
            sync_catalog_switch_save_after_exit(profile, content_stem, title_id, uses_m3m_map);
        auto tid = std::string(title_id);
        if (tid.empty()) {
            tid = resolve_switch_title_id_for_catalog(
                profile, content_stem, {}, uses_m3m_map);
        }
        if (!tid.empty()) {
            merge_ryujinx_game_cache_to_shared(
                profile.user_directory / "ryujinx" / "xdg-config" / "Ryujinx",
                tid);
        }
        if (!leaf.empty()) {
            return {leaf};
        }
    }
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
    std::string tid{ctx.title_id};
    if (!ctx.content_stem.empty()) {
        if (tid.empty()) {
            tid = resolve_switch_title_id_for_catalog(
                ctx.save_profile, ctx.content_stem, {}, ctx.uses_m3m_map);
        }
        const auto leaf = sync_catalog_switch_save_for_launch(
            ctx.save_profile, ctx.content_stem, tid, ctx.uses_m3m_map);
        result.synced_title_count = leaf.empty() ? 0 : 1;
    } else {
        const auto synced = sync_switch_shared_saves_for_profile(ctx.save_profile);
        result.synced_title_count = synced.size();
    }
    if (result.ryujinx_profile.has_value() && !tid.empty()) {
        seed_ryujinx_game_cache_from_shared(result.ryujinx_profile->data_root, tid);
    }
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
    if (env.pad_plan.has_value()) {
        log_pad_plan(*env.pad_plan, slot_index);
    }
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
        std::cout
            << "Ryujinx mode:       " << (prep.ryujinx_docked_mode ? "docked" : "handheld")
            << "\nRyujinx resolution: " << scale << "x native\n";
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
