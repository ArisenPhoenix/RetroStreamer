#include "host/host_concurrent_lobby.hpp"

#include "host/active_session_slot.hpp"
#include "host/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/host_session_hub.hpp"
#include "host/input_router_demux.hpp"
#include "host/launch_environment.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/network_input_receiver.hpp"
#include "host/session_lobby.hpp"
#include "host/streaming_audio_sink.hpp"
#include "common/platform/default_platform.hpp"
#include "common/serialization.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace archstreamer {

int run_concurrent_session_host(
    HostAppConfig config,
    GameCatalog& catalog,
    const GameList& list,
    StreamingAudioSink& streaming_audio,
    std::optional<ControllerDevice> bridge_device,
    const std::function<bool()>& should_stop) {
        if (!config.control_port.has_value()) {
            throw std::runtime_error("concurrent session host requires --control-port");
        }
        if (!config.input_port.has_value()) {
            config.input_port = 45454;
        }

        const auto max_slots = clamp_max_session_slots(config.clients);
        std::cout
            << "Concurrent session host on TCP " << *config.control_port
            << " (max slots " << static_cast<int>(max_slots)
            << ", UDP input " << *config.input_port << ").\n"
            << "Singleplayer clients each get their own emulator/stream; "
            << "Multiplayer still uses one shared-emulator lobby.\n";

        HostSessionHub hub;
        InputRouterDemux demux;
        NetworkInputReceiver network_receiver(*config.input_port, demux);
        network_receiver.start();

        TcpListener listener(*config.control_port);
        std::vector<std::unique_ptr<ActiveSessionSlot>> slots;
        int next_slot_index = 0;

        const auto art_root = config.art_root.empty()
            ? (config.rom_root.parent_path() / "Art")
            : config.art_root;

        auto erase_finished = [&] {
            slots.erase(
                std::remove_if(
                    slots.begin(),
                    slots.end(),
                    [](const std::unique_ptr<ActiveSessionSlot>& slot) {
                        if (slot == nullptr || !slot->finished()) {
                            return false;
                        }
                        std::cout
                            << "Session slot " << slot->slot_index()
                            << " finished; host lobby still accepting clients "
                               "(Stop Host to shut down).\n";
                        slot->join();
                        return true;
                    }),
                slots.end());
        };

        auto live_count = [&] {
            erase_finished();
            return hub.live_slot_count();
        };

        auto start_slot = [&](SessionPlan plan) {
            erase_finished();
            if (live_count() >= max_slots) {
                send_error_to_session_clients(plan, "host is at max concurrent sessions");
                throw std::runtime_error("host is at max concurrent sessions");
            }
            if (!config.ignore_controller.has_value()) {
                config.ignore_controller = sdl_ignore_list_for_session(plan);
            }
            auto launch_plan = launch_plan_for_session(plan);
            ActiveSessionSlotConfig slot_cfg;
            slot_cfg.slot_index = next_slot_index++;
            slot_cfg.host_config = config;
            slot_cfg.plan = std::move(plan);
            slot_cfg.launch_plan = std::move(launch_plan);
            slot_cfg.catalog = &catalog;
            slot_cfg.game_list = list;
            slot_cfg.hub = &hub;
            slot_cfg.input_demux = &demux;
            slot_cfg.streaming_audio = &streaming_audio;
            slot_cfg.bridge_device = bridge_device;
            slot_cfg.should_stop = should_stop;

            auto slot = std::make_unique<ActiveSessionSlot>(std::move(slot_cfg));
            std::cout
                << "Starting session slot " << slot->slot_index()
                << " mode=" << session_mode_name(slot->plan().session_mode)
                << " game=" << slot->plan().selected_game_id
                << " save_user=" << slot->plan().save_username << '\n';
            slot->start();
            slots.push_back(std::move(slot));
        };

        while (!should_stop()) {
            try {
                erase_finished();

                auto accepted = try_accept_control_hello(
                    listener,
                    list,
                    art_root,
                    [&] {
                        ActiveSessionInfo info;
                        info.active = live_count() > 0;
                        info.video_enabled = config.video;
                        info.audio_enabled = config.audio;
                        if (info.active && !slots.empty()) {
                            // Summarize first live slot for discovery UIs.
                            for (const auto& slot : slots) {
                                if (slot == nullptr || slot->finished()) {
                                    continue;
                                }
                                info.selected_game_id = slot->plan().selected_game_id;
                                info.session_mode = slot->plan().session_mode;
                                info.player_count = static_cast<std::uint8_t>(
                                    assigned_player_count(slot->plan().seats));
                                break;
                            }
                        }
                        return info;
                    });

                if (accepted.has_value() && accepted->have_hello) {
                    auto hello = std::move(accepted->hello);
                    auto stream = std::move(accepted->stream);
                    bool handed_off = false;
                    try {
                        // Reconnect to an existing seat.
                        if (hello.requested_players > 0) {
                            if (auto* slot = hub.slot_for_reconnect(hello); slot != nullptr) {
                                slot->enqueue_join(std::move(stream), std::move(hello), true);
                                handed_off = true;
                                continue;
                            }
                        }

                        // Late viewer into a matching live session.
                        if (hello.requested_players == 0) {
                            if (auto* slot = hub.slot_for_late_viewer(hello); slot != nullptr) {
                                slot->enqueue_join(std::move(stream), std::move(hello), false);
                                handed_off = true;
                                continue;
                            }
                            throw std::runtime_error(
                                "no active session matches that game for late viewer join");
                        }

                        // New Multiplayer lobby (only when no slots are live).
                        if (hello.session_mode == GameSessionMode::Multiplayer) {
                            if (live_count() > 0) {
                                throw std::runtime_error(
                                    "cannot start Multiplayer while singleplayer session slots are active");
                            }
                            SessionClientConnection first{
                                hub.allocate_client_id(),
                                hello,
                                std::move(stream),
                            };
                            handed_off = true;
                            auto plan = gather_session_clients(
                                listener,
                                max_slots,
                                list,
                                std::chrono::seconds(config.session_timeout_seconds),
                                std::nullopt,
                                should_stop,
                                art_root,
                                std::move(first));
                            start_slot(std::move(plan));
                            continue;
                        }

                        // New Singleplayer slot.
                        if (hub.has_multiplayer_slot()) {
                            throw std::runtime_error(
                                "cannot start Singleplayer while a Multiplayer session is active");
                        }
                        if (live_count() >= max_slots) {
                            throw std::runtime_error(
                                "host is at max concurrent singleplayer sessions "
                                "(raise Max clients / --clients)");
                        }

                        auto plan = make_singleplayer_session_plan(
                            hub.allocate_client_id(),
                            std::move(hello),
                            std::move(stream),
                            list);
                        handed_off = true;
                        start_slot(std::move(plan));
                    } catch (const std::exception& error) {
                        if (!handed_off) {
                            try {
                                stream.send_packet(serialize_packet(ErrorPacket{error.what()}));
                            } catch (const std::exception&) {
                            }
                        }
                        std::cerr << "Rejected session start: " << error.what() << '\n';
                    }
                }
            } catch (const std::exception& error) {
                // Never let a transient lobby/accept failure kill host_runner —
                // only Stop Host / SIGTERM should exit the accept loop.
                if (should_stop()) {
                    break;
                }
                std::cerr << "Host lobby error (staying up): " << error.what() << '\n';
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        for (auto& slot : slots) {
            if (slot != nullptr) {
                slot->request_stop();
            }
        }
        for (auto& slot : slots) {
            if (slot != nullptr) {
                slot->join();
            }
        }
        network_receiver.stop();
        streaming_audio.restore_default_sink();
        cleanup_x11_capture_runtime_dir();
        std::cout << "Host stopped.\n";
        return 0;
    }

} // namespace archstreamer
