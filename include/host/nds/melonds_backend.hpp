#pragma once

#include "host/gpu_select.hpp"
#include "host/launch_environment.hpp"
#include "host/nds/melonds_ctrl_client.hpp"
#include "host/nds/melonds_user_profile.hpp"
#include "host/retroarch_process.hpp"
#include "host/save_profile.hpp"
#include "host/standalone_emulator.hpp"
#include "host/virtual_joypad_resolve.hpp"
#include "common/protocol.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer {

/** Inputs for melonDS standalone session preparation. */
struct MelonDsBackendPrepContext {
    const SaveProfile& save_profile;
    std::size_t players = 1;
    bool verbose = false;
    std::uint16_t product_id_base = 0;
    std::string ignore_controller;
    bool virtualgl_capture = false;
    bool gamescope_capture = false;
    int slot_index = 0;
    std::string profile_display_name;
    DisplayLayoutPreference display_layout = DisplayLayoutPreference::Auto;
    std::vector<ArchStreamerSdlPad> resolved_pads;
};

struct MelonDsBackendPrepResult {
    MelonDsUserProfile profile;
    std::vector<ArchStreamerSdlPad> resolved_pads;
};

/**
 * Isolated NDS standalone backend. Session/host/link code should only talk to
 * this type (plus MelonDsRuntime resolve helpers) — not melonDS toml/ctrl details.
 */
class MelonDsBackend {
public:
    const char* name() const { return "melonDS"; }

    MelonDsBackendPrepResult prepare(
        RetroArchLaunchConfig& launch_config,
        MelonDsBackendPrepContext ctx);

    void assign_launch_env_profile(
        EmulatorLaunchEnvRequest& env,
        MelonDsBackendPrepResult& prep) const;

    const MelonDsUserProfile* profile() const {
        return profile_.has_value() ? &*profile_ : nullptr;
    }

    /** Mid-session LAN via patched melonDS control socket (no process restart). */
    bool lan_host(std::string_view player_name, int num_players = 2) const;
    bool lan_connect(std::string_view player_name, std::string_view host = "127.0.0.1") const;
    bool lan_end() const;
    const std::string& last_ctrl_error() const { return last_ctrl_error_; }

    std::vector<std::string> post_exit_sync(const SaveProfile& profile) const;

private:
    std::optional<MelonDsUserProfile> profile_;
    mutable std::string last_ctrl_error_;
};

bool melonds_runtime_available();
std::string melonds_unavailable_message();
std::filesystem::path default_melonds_runtime_root();
std::optional<ResolvedStandaloneEmulator> resolve_melonds_runtime();

std::unique_ptr<MelonDsBackend> make_melonds_backend();

void log_melonds_backend_prep(
    const MelonDsBackend& backend,
    const EmulatorLaunchEnvRequest& env,
    const MelonDsBackendPrepResult& prep,
    std::optional<int> slot_index = std::nullopt);

} // namespace archstreamer
