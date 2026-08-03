#include "host/nds/melonds_backend.hpp"

#include "host/nds/default_melonds_platform.hpp"
#include "host/nds/melonds_ctrl_client.hpp"
#include "host/nds/melonds_user_profile.hpp"
#include "host/virtual_joypad_resolve.hpp"

#include <iostream>
#include <utility>

namespace archstreamer {

bool melonds_runtime_available() {
    return MelonDsRuntime::available();
}

std::string melonds_unavailable_message() {
    return MelonDsRuntime::unavailable_message();
}

std::filesystem::path default_melonds_runtime_root() {
    return MelonDsRuntime::runtime_root();
}

std::optional<ResolvedStandaloneEmulator> resolve_melonds_runtime() {
    if (!MelonDsRuntime::available()) {
        return std::nullopt;
    }
    return MelonDsRuntime::ensure();
}

std::unique_ptr<MelonDsBackend> make_melonds_backend() {
    return std::make_unique<MelonDsBackend>();
}

MelonDsBackendPrepResult MelonDsBackend::prepare(
    RetroArchLaunchConfig& launch_config,
    MelonDsBackendPrepContext ctx) {
    MelonDsBackendPrepResult result;
    result.resolved_pads = std::move(ctx.resolved_pads);
    if (result.resolved_pads.empty()) {
        result.resolved_pads = find_archstreamer_sdl_pads(
            ctx.players,
            ctx.ignore_controller,
            ctx.verbose,
            ctx.product_id_base);
    }

    const auto exclusive = resolve_exclusive_archstreamer_pads(
        ctx.players,
        ctx.verbose,
        ctx.product_id_base,
        result.resolved_pads);
    result.resolved_pads = exclusive.pads;

    MelonDsProfileSeed seed;
    seed.display_layout = ctx.display_layout;
    apply_melonds_pad_seed(seed, result.resolved_pads, exclusive.sdl_device_filter);

    result.profile = prepare_melonds_user_profile(
        ctx.save_profile,
        ctx.profile_display_name.empty() ? ctx.save_profile.username : ctx.profile_display_name,
        ctx.slot_index,
        seed);
    profile_ = result.profile;

    launch_config.quiet_stdio = !ctx.verbose;
    launch_config.standalone_args_before_content = {
        "--fullscreen",
        "--archstreamer-ctrl",
        result.profile.ctrl_server_name,
    };

    (void)ctx.virtualgl_capture;
    (void)ctx.gamescope_capture;
    return result;
}

void MelonDsBackend::assign_launch_env_profile(
    EmulatorLaunchEnvRequest& env,
    MelonDsBackendPrepResult& prep) const {
    env.melonds_profile = std::move(prep.profile);
}

bool MelonDsBackend::lan_host(std::string_view player_name, int num_players) const {
    if (!profile_.has_value()) {
        last_ctrl_error_ = "melonDS profile not prepared";
        return false;
    }
    MelonDsCtrlClient client(profile_->ctrl_server_name);
    const bool ok = client.lan_host(
        player_name.empty() ? profile_->player_name : std::string(player_name),
        num_players);
    if (!ok) {
        last_ctrl_error_ = client.last_error();
    }
    return ok;
}

bool MelonDsBackend::lan_connect(std::string_view player_name, std::string_view host) const {
    if (!profile_.has_value()) {
        last_ctrl_error_ = "melonDS profile not prepared";
        return false;
    }
    MelonDsCtrlClient client(profile_->ctrl_server_name);
    const bool ok = client.lan_connect(
        player_name.empty() ? profile_->player_name : std::string(player_name),
        host);
    if (!ok) {
        last_ctrl_error_ = client.last_error();
    }
    return ok;
}

bool MelonDsBackend::lan_end() const {
    if (!profile_.has_value()) {
        last_ctrl_error_ = "melonDS profile not prepared";
        return false;
    }
    MelonDsCtrlClient client(profile_->ctrl_server_name);
    const bool ok = client.lan_end();
    if (!ok) {
        last_ctrl_error_ = client.last_error();
    }
    return ok;
}

std::vector<std::string> MelonDsBackend::post_exit_sync(const SaveProfile& profile) const {
    (void)profile;
    return {};
}

void log_melonds_backend_prep(
    const MelonDsBackend& backend,
    const EmulatorLaunchEnvRequest& env,
    const MelonDsBackendPrepResult& prep,
    std::optional<int> slot_index) {
    (void)backend;
    (void)prep;
    if (!env.melonds_profile.has_value()) {
        return;
    }
    const auto& profile = *env.melonds_profile;
    if (slot_index.has_value()) {
        std::cout
            << "session slot " << *slot_index << ": melonDS"
            << " config=" << profile.config_path
            << " ctrl=" << profile.ctrl_server_name
            << " player=" << profile.player_name << '\n';
        return;
    }
    std::cout
        << "melonDS config: " << profile.config_path
        << " ctrl=" << profile.ctrl_server_name
        << " player=" << profile.player_name << '\n';
}

} // namespace archstreamer
