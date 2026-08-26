#include "common/catalog_presenter.hpp"
#include "common/cli_common.hpp"
#include "common/participant_role.hpp"
#include "host/emulator_orphan_reaper.hpp"
#include "host/console/game_catalog.hpp"
#include "archstreamer/runtime_cadence/cadence.hpp"
#include "host/session/active_session_slot.hpp"
#include "host/db/cadence_resource_lease.hpp"
#include "host/db/cadence_session_events.hpp"
#include "host/session/direct_session_launch.hpp"
#include "host/console/game_catalog_scanner.hpp"
#include "host/db/game_meta_store.hpp"
#include "host/host_app.hpp"
#include "host/host_app_config.hpp"
#include "host/lobby/host_concurrent_lobby.hpp"
#include "host/hardware/streaming_audio_sink.hpp"
#include "host/host_launch_planner.hpp"
#include "host/lobby/host_session_helpers.hpp"
#include "host/virtual/input_router.hpp"
#include "host/virtual/virtual_keyboard.hpp"
#include "host/hardware/local_controller_bridge.hpp"
#include "host/virtual/network_input_receiver.hpp"
#include "host/hardware/default_host_platform.hpp"
#include "host/hardware/gpu_select.hpp"
#include "host/session/launch_assemble.hpp"
#include "host/session/launch_types.hpp"
#include "host/session/run_helpers.hpp"
#include "host/session/lobby.hpp"
#include "host/session/runtime.hpp"
#include "host/virtual/virtual_gamepad.hpp"

#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

std::string startup_quote(std::string value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

const char* gpu_match_kind_name(GpuSelectionMatchKind kind) {
    switch (kind) {
    case GpuSelectionMatchKind::Auto:
        return "auto";
    case GpuSelectionMatchKind::Exact:
        return "exact";
    case GpuSelectionMatchKind::Fuzzy:
        return "fuzzy";
    case GpuSelectionMatchKind::None:
    default:
        return "none";
    }
}

void log_startup_available_gpus(const std::vector<GpuDevice>& devices) {
    std::cout << "[archstreamer-startup] available-gpus=";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (i > 0) {
            std::cout << ';';
        }
        std::cout << devices[i].id << ' ' << startup_quote(devices[i].name);
    }
    std::cout << '\n';
}

void log_startup_gpu_match(
    std::string_view role,
    const std::string& requested,
    const GpuSelectionResult& result) {
    std::cout
        << "[archstreamer-startup] gpu-role=" << role
        << " gpu-request=" << startup_quote(requested.empty() ? "auto" : requested)
        << " gpu-match=" << gpu_match_kind_name(result.match_kind);
    if (result.device.has_value()) {
        std::cout
            << " selected=" << result.device->id
            << " name=" << startup_quote(result.device->name);
        if (result.device->nvidia_index >= 0) {
            std::cout << " nvidia-index=" << result.device->nvidia_index;
        }
        if (result.device->vulkan_index >= 0) {
            std::cout << " vulkan-index=" << result.device->vulkan_index;
        }
    }
    std::cout << '\n';
}

} // namespace

HostApp::HostApp(HostAppConfig config)
    : config_(std::move(config)) {
}

void HostApp::apply_host_player_media_policy(HostAppConfig& config, bool host_plays_locally) {
    // Host Player is local RetroArch on the real display/speakers. Streaming the
    // same session captures the playback sink monitor (or fights virtio-evdev for
    // the pad) and sounds/feels broken — force media off regardless of CLI/GUI.
    if (host_plays_locally && (config.audio || config.video)) {
        std::cout
            << "Host Player: disabling stream video/audio for local play "
            << "(use Host Viewer to stream).\n";
        config.audio = false;
        config.video = false;
    }
}

void HostApp::prepare_streaming_audio_source(HostAppConfig& config, StreamingAudioSink& sink) {
    if (!config.audio || !config.audio_source.empty()) {
        return;
    }
    // Concurrent lobby uses archstreamer-0..N per session slot. Creating the
    // legacy "archstreamer" sink here only clutters the system mixer.
    if (config.control_port.has_value()) {
        sink.prune_unused(
            static_cast<int>(clamp_max_session_slots(config.clients)),
            /*keep_legacy=*/false);
        sink.restore_default_sink();
        std::cout
            << "Audio capture: per-slot null sinks (archstreamer-0…); "
            << "host speakers stay quiet unless Watch stream locally\n";
        return;
    }
    try {
        config.audio_source = sink.monitor_source();
        std::cout
            << "Audio capture: " << config.audio_source
            << " (null sink; host speakers stay quiet unless Watch stream locally)\n";
    } catch (const std::exception& error) {
        config.audio_source = StreamingAudioSink::default_monitor_source();
        std::cerr << "Warning: " << error.what() << '\n';
        if (config.audio_source.empty()) {
            std::cerr
                << "Warning: could not determine audio monitor source; "
                << "audio capture will use the audio server default source.\n";
        } else {
            std::cerr
                << "Warning: falling back to default sink monitor "
                << config.audio_source
                << " (host may hear game audio locally).\n";
        }
    }
}

std::optional<ControllerDevice> HostApp::resolve_bridge_device(const HostAppConfig& config) {
    if (config.host_role == ParticipantRole::Viewer && config.bridge_controller_index.has_value()) {
        throw std::runtime_error("--bridge-controller cannot be used with --host-role viewer");
    }
    if (config.host_role == ParticipantRole::Player && !config.bridge_controller_index.has_value()) {
        throw std::runtime_error(
            "--host-role player requires --bridge-controller "
            "(or use --host-role viewer for a dedicated streaming host)");
    }
    if (config.host_role == ParticipantRole::Player && config.bridge_controller_index.has_value()) {
        return local_bridge_device_for(*config.bridge_controller_index);
    }
    return std::nullopt;
}

int HostApp::run_lobby_sessions(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    const std::function<bool()>& should_stop) {
    return run_concurrent_session_host(
        std::move(config),
        catalog,
        list,
        streaming_audio,
        bridge_device,
        should_stop);
}

int HostApp::run_direct_session(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    std::optional<HostPlayerControllerIdentity> bridge_identity,
    bool host_plays_locally,
    const std::function<bool()>& should_stop) {
    auto backends = SessionBackendState{};
    std::optional<std::string> session_end_reason;

    auto context_result = prepare_direct_session_context(config, catalog, list, bridge_identity);
    if (!context_result.has_value()) {
        return 1;
    }
    auto context = std::move(*context_result);
    auto& launch_plan = context.launch_plan;
    auto& assets = context.assets;
    backends.system_key = assets.content.system_key;

    // Streaming already forced off above for Host Player.
    // Emulator child env is assembled once later (audio/input/gpu/capture/emulator).
    // Blacklist every physical pad currently attached so RetroArch is less likely to
    // bind P1 to a host controller. Virtual ArchStreamer pads are created after this.
    append_session_controller_ignore_list(
        config,
        bridge_device,
        {},
        /*warn_if_not_sdl2=*/true);

    // Host Player keeps the real DISPLAY (and speakers). Streamed RetroArch needs a
    // virtual capture surface. Switch standalone defaults to headless gamescope on Linux;
    // Windows captures the desktop/HWND via d3d11screencapturesrc (no gamescope).
    auto cadence = open_direct_cadence_session(launch_plan, config);
    auto lease = cadence.tracker.leases();
    lease.set_pools(cadence_resource_pools_from(config));
    auto launch_env = prepare_direct_launch_environment(
        config,
        assets.launch_config,
        host_plays_locally,
        lease);
    register_session_xtest_display(launch_env.request.session_id, launch_env.request.xtest_display);

    auto media = build_session_media_plan(
        config,
        nullptr,
        SessionMediaPlanKind::Direct);

    print_direct_launch_summary(
        config,
        launch_plan,
        assets.save_profile,
        assets.launch_config,
        media.streams,
        launch_env.capture.capture_display);

    if (config.dry_run) {
        cadence.tracker.end("dry run");
        return 0;
    }

    apply_product_id_base(launch_plan.virtual_identities, cadence.grant.pad_product_base);
    HostVirtualGamepadBus gamepads(launch_plan.virtual_identities);
    plug_session_gamepads(gamepads, launch_plan.players);
    // Virtual keyboard targets ARCHSTREAMER_XTEST_DISPLAY for gamescope, else Xvfb capture.
    VirtualKeyboard keyboard(launch_env.request.xtest_display);
    wait_for_session_input_enumeration();

    auto devices = resolve_session_device_plan(
        config,
        launch_plan,
        SessionPadPlanKind::Direct,
        cadence.grant.pad_product_base);
    apply_capture_to_session_device_plan(devices, config, launch_env.capture);
    devices.capture.resolved_gpu = launch_env.gpu.resolved_gpu;

    auto backend_context = make_session_backend_prepare_context(
        config,
        launch_plan,
        assets,
        devices,
        backends,
        launch_env.capture,
        launch_env.request,
        resolve_direct_session_participants(assets.save_profile.username));
    prepare_direct_backend(backend_context, keyboard, cadence.grant.netcmd_port);

    apply_capture_and_launch_environment(
        assets.launch_config,
        launch_env.capture,
        config,
        launch_env.gpu.gamescope_vk_device,
        launch_env.gpu.resolved_gpu,
        launch_env.request);

    if (devices.capture.capture_fullscreen) {
        std::cout << devices.capture_info();
    }

    // Pin Viewer RetroArch to the capture null sink (speakers stay quiet unless Watch-local).
    if (config.audio) {
        park_session_game_audio(&streaming_audio);
    }

    InputRouter input_router(gamepads, &keyboard);
    auto melonds_touch_ctrl = configure_session_input_router(
        input_router,
        keyboard,
        launch_plan,
        backends);
    print_input_seats(launch_plan.seats);

    auto network_receiver = std::optional<NetworkInputReceiver>{};
    if (config.input_port.has_value()) {
        network_receiver.emplace(*config.input_port, input_router);
    }

    auto media_server = start_host_media_server_if_needed(HostMediaStartRequest{
        config,
        SessionMediaCaptureContext{
            devices.capture.capture_display,
            devices.capture.display_backend,
            launch_env.gpu.nvenc_cuda_device_id,
        },
        SessionMediaStreamContext{
            media.config,
            media.destinations,
            media.streams,
        },
    });
    // Plug after Xvfb/Xephyr is up. Soft-fail so a keyboard issue never kills the session.
    if (!plug_virtual_keyboard_with_retry(
            keyboard,
            devices.capture.use_virtual_capture,
            launch_env.capture.gamescope_capture)) {
        if (devices.capture.use_virtual_capture && !launch_env.capture.gamescope_capture) {
            std::cerr << "Warning: continuing without remoted keyboard (pads still work).\n";
        }
    }

    auto local_bridge = std::optional<LocalControllerBridge>{};
    if (bridge_device.has_value()) {
        local_bridge.emplace(*bridge_device);
    }

    auto session_runtime = make_session_runtime(launch_plan);
    session_runtime->bind_launch_config(std::move(assets.launch_config));
    std::cout << session_runtime->info();

    log_direct_emulator_command(session_runtime->launch_config(), assets.resolved_retroarch);
    start_emulator_and_verify(*session_runtime, EmulatorStartFailDetail::DirectCli);
    post_emulator_start_warmup(
        media_server.get(),
        config,
        media.streams,
        *session_runtime,
        &streaming_audio,
        std::nullopt,
        gamepads,
        launch_plan.players,
        &keyboard,
        launch_env.capture.gamescope_capture,
        launch_env.request.xtest_display);

    if (launch_env.capture.gamescope_capture && keyboard.plugged()) {
        const auto actual = keyboard.capture_display();
        if (!actual.empty() && actual != launch_env.request.xtest_display) {
            if (lease.replace(cadence::resource::kXtestDisplay, actual)) {
                launch_env.request.xtest_display = actual;
                register_session_xtest_display(launch_env.request.session_id, actual);
            }
        }
    }
    attach_direct_cadence_emulator(cadence.tracker, launch_plan, *session_runtime);

    if (devices.keyboard.arm_soft_keyboard && devices.keyboard.standalone_soft_keyboard) {
        std::string display = launch_env.request.xtest_display;
        if (keyboard.plugged()) {
            display = keyboard.capture_display();
        }
        schedule_soft_keyboard(
            devices.keyboard.standalone_soft_keyboard,
            devices.keyboard.soft_keyboard_fallback,
            // Prefer OCR of Ryujinx HeaderText when the dialog appears.
            {},
            display,
            session_runtime->emulator().process_id().value_or(0));
    }

    // Start UDP input after the optional A-pulse so it does not race uinput updates.
    if (network_receiver.has_value()) {
        network_receiver->start();
    }

    SessionLoopCadence loop_cadence(
        local_bridge.has_value() ? &*local_bridge : nullptr,
        &input_router,
        &streaming_audio,
        std::nullopt,
        config.audio);
    while (!should_stop() && session_runtime->emulator_running()) {
        loop_cadence.tick();
    }

    if (!should_stop() && !session_end_reason.has_value() && !session_runtime->emulator_running()) {
        const auto code = session_runtime->last_exit_code().value_or(-1);
        const auto stderr_tail = session_runtime->last_stderr_tail();
        std::ostringstream reason;
        reason << format_emulator_exit_summary(code);
        if (session_runtime->launch_config().standalone && launch_env.capture.gamescope_capture) {
            reason << " — if Host GPU is the non-boot NVIDIA, check Gamescope WSI "
                      "(ENABLE_GAMESCOPE_WSI / VK_ADD_IMPLICIT_LAYER_PATH); "
                      "Switch emulators often log \"Device lacks a present queue\"";
        }
        session_end_reason = reason.str();
        std::cerr << "Stopping session: " << *session_end_reason << '\n';
        if (!stderr_tail.empty()) {
            std::cerr << "emulator/gamescope stdio tail:\n" << stderr_tail << '\n';
        } else {
            std::cerr << "(no emulator/gamescope stdio captured)\n";
        }
    }

    if (network_receiver.has_value()) {
        network_receiver->stop();
    }

    // Close XTest before stopping gamescope/Xvfb so Xlib does not abort the process.
    unplug_session_keyboard(&keyboard);
    unregister_session_xtest_display(launch_env.request.session_id);

    untrack_session_audio(&streaming_audio);
    stop_session_runtime(session_runtime);
    if (session_runtime != nullptr) {
        if (const auto code = session_runtime->last_exit_code(); code.has_value()) {
            std::cout << format_emulator_exit_summary(*code) << '\n';
            if (*code == 127) {
                std::cerr
                    << "hint: exit 127 usually means the RetroArch launcher was not found. "
                    << "On Bazzite install: flatpak install flathub org.libretro.RetroArch\n";
            }
        }
    }
    if (backends.switch_backend) {
        sync_and_log_post_exit_switch_saves(
            assets.save_profile,
            std::nullopt,
            backends.switch_backend.get(),
            backends.switch_launch_content_stem,
            backends.switch_launch_title_id,
            backends.switch_launch_uses_m3m_map);
    }
    if (backends.melonds_backend) {
        (void)backends.melonds_backend->post_exit_sync(assets.save_profile);
    }
    const std::string end_reason = should_stop()
        ? "host stopped"
        : session_end_reason.value_or("session ended");

    end_direct_cadence_session(cadence.tracker, launch_plan, end_reason);
    stop_session_media(media_server);
    if (config.audio) {
        streaming_audio.restore_default_sink();
    }
    cleanup_x11_capture_runtime_dir();

    return 0;
}

int HostApp::run(const std::function<bool()>& should_stop) {
    try {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        std::cout
            << "[archstreamer-startup] config-state"
            << " control-port=" << (config_.control_port.has_value()
                ? std::to_string(*config_.control_port)
                : std::string("none"))
            << " input-port=" << (config_.input_port.has_value()
                ? std::to_string(*config_.input_port)
                : std::string("none"))
            << " video-port=" << config_.video_port
            << " audio-port=" << config_.audio_port
            << " virtual-display=" << startup_quote(config_.virtual_display)
            << " gpu=" << startup_quote(config_.encode_gpu)
            << " render-gpu=" << startup_quote(effective_render_gpu_selection(config_))
            << " log-root=" << startup_quote(config_.log_root.string())
            << '\n';
        {
            const auto devices = list_render_gpus();
            log_startup_available_gpus(devices);
            log_startup_gpu_match(
                "encode",
                config_.encode_gpu,
                resolve_render_gpu_with_match_from(devices, config_.encode_gpu));
            const auto render_request = effective_render_gpu_selection(config_);
            log_startup_gpu_match(
                "render",
                render_request,
                resolve_render_gpu_with_match_from(devices, render_request));
        }

        // Last resort only: SessionRuntime RAII owns kill for live sessions.
        // This reaps leftovers from a prior host that never got to run destructors.
        reap_orphaned_emulator_processes();

        {
            auto cadence = archstreamer::cadence::make_runtime_store();
            if (cadence->ensure_ready()) {
                archstreamer::cadence::RuntimeEvent started;
                started.kind = "host_started";
                started.host_id = cadence_host_id();
                started.detail = "host_runner";
                (void)cadence->record_event(started);

                const auto reaped = archstreamer::cadence::reap_stale_instance_state(
                    *cadence,
                    started.host_id);
                if (reaped.sessions_ended > 0 || reaped.claims_released > 0 ||
                    reaped.connections_ended > 0) {
                    std::cout
                        << "cadence: reaped stale state ("
                        << reaped.sessions_ended << " session(s), "
                        << reaped.claims_released << " claim(s), "
                        << reaped.connections_ended << " connection(s))\n";
                }

                const auto save_root = config_.save_root.empty()
                    ? default_save_profile_root()
                    : config_.save_root;
                const auto host_name = !config_.host_name.empty()
                    ? config_.host_name
                    : (config_.username.empty() ? default_cli_username() : config_.username);
                if (!archstreamer::cadence::canonical_identity_name(host_name).empty()) {
                    archstreamer::cadence::HostRecord host;
                    host.identity_id = archstreamer::cadence::identity_id_from_name(host_name);
                    host.host_name = host_name;
                    host.display_name = host_name;
                    host.save_root = save_root.lexically_normal().string();
                    (void)cadence->upsert_host(host);
                }
                const auto imported =
                    archstreamer::cadence::import_users_from_save_root(*cadence, save_root);
                if (imported > 0) {
                    std::cout
                        << "cadence: imported " << imported
                        << " user(s) from save profiles\n";
                }
                const auto backfilled =
                    archstreamer::cadence::backfill_user_profile_paths(*cadence, save_root);
                if (backfilled > 0) {
                    std::cout
                        << "cadence: backfilled profile paths for "
                        << backfilled << " user(s)\n";
                }
            }
        }

        const auto host_id_for_shutdown = cadence_host_id();
        auto release_this_host = [&](std::string_view reason) {
            try {
                auto cadence = archstreamer::cadence::make_runtime_store();
                if (!cadence->ensure_ready()) {
                    return;
                }
                const auto released = archstreamer::cadence::release_host_instance_state(
                    *cadence,
                    host_id_for_shutdown,
                    reason);
                if (released.sessions_ended > 0 || released.claims_released > 0 ||
                    released.connections_ended > 0) {
                    std::cout
                        << "cadence: host shutdown released "
                        << released.sessions_ended << " session(s), "
                        << released.claims_released << " claim(s), "
                        << released.connections_ended << " connection(s)\n";
                }
            } catch (...) {
            }
        };

        auto config = config_;
        std::vector<CatalogScanIssue> scan_issues;
        auto catalog = scan_game_catalog(
            config.rom_root,
            LibretroCoreRegistry::ubuntu_defaults(),
            config.meta_root,
            &scan_issues);
        for (const auto& issue : scan_issues) {
            // Already printed to stderr inside scan; summarize count for operators.
            (void)issue;
        }
        if (!scan_issues.empty()) {
            std::cerr
                << "host: " << scan_issues.size()
                << " title(s) locked (missing Meta, ROM stem mismatch, or invalid .m3m); "
                   "not hosted until fixed.\n";
        }
        const auto list = catalog.list();
        rebuild_catalog_offerings_from_list(list);

        const bool host_plays_locally =
            config.host_role == ParticipantRole::Player &&
            config.bridge_controller_index.has_value();
        apply_host_player_media_policy(config, host_plays_locally);

        StreamingAudioSink streaming_audio;
        prepare_streaming_audio_source(config, streaming_audio);

        if (list.games.empty()) {
            std::cerr << "No supported games found under " << config.rom_root << '\n';
            release_this_host("host exit");
            return 1;
        }

        if (config.list || (!config.selector.has_value() && !config.control_port.has_value())) {
            std::cout << "Found " << list.games.size() << " supported games under " << config.rom_root << ".\n";
            print_game_catalog(std::cout, list);
            release_this_host("host exit");
            return config.list ? 0 : 2;
        }

        auto bridge_device = resolve_bridge_device(config);
        auto bridge_identity = std::optional<HostPlayerControllerIdentity>{};
        if (bridge_device.has_value()) {
            bridge_identity = host_player_controller_identity(*bridge_device);
        }

        int result = 0;
        if (config.control_port.has_value()) {
            if (should_stop()) {
                release_this_host("host stopped");
                return 0;
            }
            result = run_lobby_sessions(config, catalog, list, streaming_audio, bridge_device, should_stop);
        } else {
            result = run_direct_session(
                config,
                catalog,
                list,
                streaming_audio,
                bridge_device,
                bridge_identity,
                host_plays_locally,
                should_stop);
        }
        release_this_host(should_stop() ? "host stopped" : "host exit");
        return result;
    } catch (const std::exception& error) {
        StreamingAudioSink{}.restore_default_sink();
        cleanup_x11_capture_runtime_dir();
        try {
            auto cadence = archstreamer::cadence::make_runtime_store();
            if (cadence->ensure_ready()) {
                (void)archstreamer::cadence::release_host_instance_state(
                    *cadence,
                    cadence_host_id(),
                    should_stop() ? "host stopped" : "host error");
            }
        } catch (...) {
        }
        if (should_stop()) {
            std::cout << "Host stopped.\n";
            return 0;
        }
        std::cerr << "host_runner: " << error.what() << '\n';
        return 1;
    }
}

} // namespace archstreamer
