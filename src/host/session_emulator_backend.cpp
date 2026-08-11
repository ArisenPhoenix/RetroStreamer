#include "host/session_emulator_backend.hpp"

#include "host/retroarch_config_writer.hpp"
#include "host/session_launch_assemble.hpp"
#include "host/soft_keyboard_host.hpp"
#include "host/switch/switch_backend.hpp"
#include "host/virtual_keyboard.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace archstreamer {
namespace {

class SwitchSessionEmulatorBackend final : public SessionEmulatorBackend {
public:
    const char* name() const override { return "switch"; }

    SessionBackendPrepareResult prepare(
        SessionBackendPrepareContext& context,
        const SessionBackendPrepareOptions& options) override {
        SessionBackendPrepareResult result;
        auto& config = context.config;
        auto& user = context.user;
        auto& game = context.game;
        auto& video = context.video;
        auto& devices = context.input.devices;
        auto& backends = context.backends;
        auto& launch_env_request = context.launch_env_request;

        if (!backends.switch_backend) {
            throw std::runtime_error("Switch backend selected without runtime backend");
        }
        if (options.keyboard != nullptr) {
            options.keyboard->set_switch_style_hotkeys(true);
        }

        const auto switch_content = resolve_switch_launch_content(
            user.save_profile,
            game.launch_config,
            game.content);
        auto switch_prep = backends.switch_backend->prepare(
            game.launch_config,
            SwitchBackendPrepContext{
                user.save_profile,
                game.launch_plan.players,
                config.verbose,
                devices.input.product_id_base,
                config.ignore_controller.value_or(""),
                config.graphics_api,
                video.capture.virtualgl_capture,
                options.gamescope_capture,
                video.switch_scale,
                options.prefer_switch_handheld_mode,
                &video.devices.resolved_gpu,
                user.participants.profile_display_name,
                std::move(devices.input.resolved_pads),
                static_cast<std::size_t>(std::max(0, options.slot_index)),
                game.launch_plan.game_id,
                switch_content.content_stem,
                switch_content.title_id,
            });
        devices.input.resolved_pads = std::move(switch_prep.resolved_pads);
        backends.switch_backend->assign_launch_env_profile(launch_env_request, switch_prep);
        log_switch_backend_prep(
            *backends.switch_backend,
            launch_env_request,
            switch_prep,
            video.switch_scale,
            video.devices.resolved_gpu,
            options.slot_index);
        backends.switch_launch_content_stem = switch_content.content_stem;
        backends.switch_launch_title_id = switch_content.title_id;
        if (backends.switch_backend->enable_soft_keyboard()) {
            if (!devices.keyboard.standalone_soft_keyboard) {
                result.soft_keyboard = std::make_shared<SoftKeyboardHostBridge>();
            } else {
                result.soft_keyboard = devices.keyboard.standalone_soft_keyboard;
            }
            devices.keyboard.soft_keyboard_fallback = user.participants.profile_display_name;
            devices.keyboard.arm_soft_keyboard = true;
        }
        return result;
    }
};

class MelonDsSessionEmulatorBackend final : public SessionEmulatorBackend {
public:
    const char* name() const override { return "melonds"; }

    SessionBackendPrepareResult prepare(
        SessionBackendPrepareContext& context,
        const SessionBackendPrepareOptions& options) override {
        auto& config = context.config;
        auto& user = context.user;
        auto& game = context.game;
        auto& video = context.video;
        auto& devices = context.input.devices;
        auto& backends = context.backends;
        auto& launch_env_request = context.launch_env_request;

        if (!backends.melonds_backend) {
            throw std::runtime_error("melonDS backend selected without runtime backend");
        }
        auto melonds_prep = backends.melonds_backend->prepare(
            game.launch_config,
            MelonDsBackendPrepContext{
                user.save_profile,
                game.launch_plan.players,
                config.verbose,
                devices.input.product_id_base,
                config.ignore_controller.value_or(""),
                video.capture.virtualgl_capture,
                options.gamescope_capture,
                options.slot_index,
                user.participants.profile_display_name,
                user.participants.display_layout,
                std::move(devices.input.resolved_pads),
            });
        devices.input.resolved_pads = std::move(melonds_prep.resolved_pads);
        backends.melonds_backend->assign_launch_env_profile(launch_env_request, melonds_prep);
        log_melonds_backend_prep(
            *backends.melonds_backend,
            launch_env_request,
            melonds_prep,
            options.slot_index);
        return {};
    }
};

class RetroArchSessionEmulatorBackend final : public SessionEmulatorBackend {
public:
    const char* name() const override { return "retroarch"; }

    SessionBackendPrepareResult prepare(
        SessionBackendPrepareContext& context,
        const SessionBackendPrepareOptions& options) override {
        auto& config = context.config;
        auto& user = context.user;
        auto& game = context.game;
        auto& video = context.video;
        auto& devices = context.input.devices;
        auto& backends = context.backends;
        auto& launch_env_request = context.launch_env_request;

        if (game.launch_config.standalone) {
            throw std::runtime_error(
                "standalone launch missing backend for system=" + game.content.system_key);
        }

        const auto override_params = build_session_retroarch_override(
            context,
            SessionRetroArchOverrideOptions{
                options.use_virtual_capture,
                options.slot_index,
                options.retroarch_netcmd_port,
                user.participants.display_layout,
            });
        const auto runtime_override =
            apply_retroarch_override(game.launch_config, override_params);
        if (options.log_retroarch_details) {
            std::cout
                << "RetroArch config: " << runtime_override
                << "\nVirtual joypad index: " << devices.input.virtual_joypad_index
                << " (driver=" << config.retroarch_joypad_driver << ")\n";
            const int scale = std::clamp(video.retroarch_scale, 1, 6);
            std::cout << "RetroArch resolution: " << scale << "x native"
                      << " (known cores via .opt)\n";
            if (!backends.system_key.empty()) {
                std::cout << "Face buttons: system=" << backends.system_key
                          << " (" << face_button_map_name(backends.system_key) << ")\n";
            }
        }
        launch_env_request.pad_plan = devices.input.shared_pad_plan;
        log_pad_plan(devices.input.shared_pad_plan, options.slot_index);
        return {};
    }
};

std::unique_ptr<SessionEmulatorBackend> select_session_emulator_backend(
    SessionBackendPrepareContext& context,
    const SessionBackendPrepareOptions& options) {
    auto& backends = context.backends;
    auto& game = context.game;
    prepare_session_standalone_backend(
        game.content.system_key,
        game.launch_config,
        backends);

    if (backends.switch_backend) {
        return std::make_unique<SwitchSessionEmulatorBackend>();
    }
    if (backends.melonds_backend) {
        return std::make_unique<MelonDsSessionEmulatorBackend>();
    }
    return std::make_unique<RetroArchSessionEmulatorBackend>();
}

} // namespace

SessionBackendPrepareResult prepare_session_emulator_backend(
    SessionBackendPrepareContext& context,
    const SessionBackendPrepareOptions& options) {
    auto backend = select_session_emulator_backend(context, options);
    return backend->prepare(context, options);
}

} // namespace archstreamer
