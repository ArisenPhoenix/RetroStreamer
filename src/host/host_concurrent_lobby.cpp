#include "host/host_concurrent_lobby.hpp"

#include "host/active_session_slot.hpp"
#include "host/emulator_orphan_reaper.hpp"
#include "host/game_catalog.hpp"
#include "host/host_app_config.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/host_session_hub.hpp"
#include "host/input_router_demux.hpp"
#include "host/launch_environment.hpp"
#include "host/local_controller_bridge.hpp"
#include "host/network_input_receiver.hpp"
#include "host/save_active_sessions.hpp"
#include "host/session_lobby.hpp"
#include "host/streaming_audio_sink.hpp"
#include "host/user_credentials.hpp"
#include "common/platform/default_platform.hpp"
#include "common/serialization.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>

namespace archstreamer {
namespace {

// Slot numbers are claimed machine-wide, so the span has to cover every host
// process on the box, not just one lobby's max_slots.
constexpr int kSlotLeaseSpan = 16;

} // namespace

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
        // Concurrent sessions use archstreamer-0..N only — drop the legacy
        // "archstreamer" sink and any higher slot leftovers from older runs.
        streaming_audio.prune_unused(static_cast<int>(max_slots), /*keep_legacy=*/false);
        // Drop stale Users-tab "Active" markers from a previous host crash.
        {
            std::error_code ec;
            const auto active_dir = active_save_sessions_directory(config.save_root);
            if (std::filesystem::is_directory(active_dir, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(active_dir, ec)) {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }
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

        struct LobbyPresenceClient {
            ClientId client_id = 0;
            std::string username;
            TcpStream stream;
        };
        std::vector<LobbyPresenceClient> presence_clients;

        const auto publish_lobby_presence = [&](const LobbyPresenceClient& client) {
            ConnectedClientPresence presence;
            presence.username = client.username;
            presence.client_id = client.client_id;
            presence.slot_index = -1;
            presence.phase = "catalog";
            presence.seated = false;
            publish_connected_client(config.save_root, presence);
        };

        const auto erase_presence_at = [&](std::size_t index) {
            if (index >= presence_clients.size()) {
                return;
            }
            clear_connected_client(
                config.save_root,
                presence_clients[index].client_id,
                -1);
            presence_clients.erase(
                presence_clients.begin() + static_cast<std::ptrdiff_t>(index));
        };

        const auto poll_lobby_presence = [&] {
            for (std::size_t i = 0; i < presence_clients.size();) {
                auto& client = presence_clients[i];
                if (auto reason = take_connected_client_disconnect_request(
                        config.save_root, client.client_id, -1);
                    reason.has_value()) {
                    std::cerr
                        << "Lobby presence " << static_cast<int>(client.client_id)
                        << " (" << client.username << ") kicked: " << *reason << '\n';
                    try {
                        client.stream = TcpStream{};
                    } catch (const std::exception&) {
                    }
                    erase_presence_at(i);
                    continue;
                }
                if (!client.stream.open() || client.stream.peer_closed()) {
                    std::cout
                        << "Lobby presence " << static_cast<int>(client.client_id)
                        << " (" << client.username << ") disconnected.\n";
                    erase_presence_at(i);
                    continue;
                }
                // Drain keepalives / stray packets so the socket stays healthy.
                try {
                    while (client.stream.readable()) {
                        const auto packet = client.stream.receive_packet();
                        if (!packet.has_value()) {
                            erase_presence_at(i);
                            goto next_presence;
                        }
                        // Ignore payload; presence is the open TCP itself.
                        (void)deserialize_packet(*packet);
                    }
                } catch (const std::exception&) {
                    erase_presence_at(i);
                    continue;
                }
                ++i;
            next_presence:
                continue;
            }
        };

        const auto art_root = config.art_root.empty()
            ? (config.rom_root.parent_path() / "Art")
            : config.art_root;

        auto erase_finished = [&] {
            const auto before = slots.size();
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
            if (slots.size() != before) {
                // Catch AppImage/flatpak stragglers whose session token was
                // unregistered after a failed group kill.
                (void)reap_stale_emulator_session_tokens();
            }
        };

        auto live_count = [&] {
            erase_finished();
            return hub.live_slot_count();
        };

        auto start_slot = [&](SessionPlan plan) {
            erase_finished();
            if (!plan.save_username.empty() &&
                hub.save_profile_active(plan.save_username)) {
                const std::string message =
                    "user " + plan.save_username +
                    " already has an active session; reconnect to it or end it first";
                send_error_to_session_clients(plan, message);
                throw std::runtime_error(message);
            }
            if (live_count() >= max_slots) {
                send_error_to_session_clients(plan, "host is at max concurrent sessions");
                throw std::runtime_error("host is at max concurrent sessions");
            }
            if (!config.ignore_controller.has_value()) {
                config.ignore_controller = sdl_ignore_list_for_session(plan);
            }
            auto lease = SessionSlotLease::claim(
                kSlotLeaseSpan,
                parse_virtual_display_number(config.virtual_display));
            if (!lease.valid()) {
                const std::string message =
                    "no free session slot on this machine (another host is using them)";
                send_error_to_session_clients(plan, message);
                throw std::runtime_error(message);
            }
            auto launch_plan = launch_plan_for_session(plan);
            ActiveSessionSlotConfig slot_cfg;
            slot_cfg.slot_index = lease.index();
            slot_cfg.slot_lease = std::move(lease);
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
                poll_lobby_presence();

                auto accepted = try_accept_control_hello(
                    listener,
                    list,
                    art_root,
                    [&] {
                        ActiveSessionInfo info;
                        info.active = live_count() > 0;
                        info.video_enabled = config.video;
                        info.audio_enabled = config.audio;
                        info.active_slots = static_cast<std::uint8_t>(live_count());
                        info.max_slots = max_slots;
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
                    },
                    config.save_root,
                    config.allow_new_users);

                if (accepted.has_value() && accepted->have_presence) {
                    // Replace any prior catalog presence for this username.
                    for (std::size_t i = 0; i < presence_clients.size();) {
                        if (presence_clients[i].username == accepted->presence.username) {
                            erase_presence_at(i);
                            continue;
                        }
                        ++i;
                    }
                    LobbyPresenceClient held;
                    held.client_id = hub.allocate_client_id();
                    held.username = accepted->presence.username;
                    held.stream = std::move(accepted->stream);
                    try {
                        held.stream.send_packet(serialize_packet(LobbyPresenceAck{held.client_id}));
                    } catch (const std::exception& error) {
                        std::cerr
                            << "Failed to ack lobby presence for " << held.username
                            << ": " << error.what() << '\n';
                        continue;
                    }
                    std::cout
                        << "Lobby presence " << static_cast<int>(held.client_id)
                        << " username=" << held.username << " (catalog Connected)\n";
                    publish_lobby_presence(held);
                    presence_clients.push_back(std::move(held));
                    continue;
                }

                if (accepted.has_value() && accepted->have_hello) {
                    auto hello = std::move(accepted->hello);
                    auto stream = std::move(accepted->stream);
                    // Drop catalog presence when the same user starts playing.
                    for (std::size_t i = 0; i < presence_clients.size();) {
                        if (presence_clients[i].username == hello.username) {
                            erase_presence_at(i);
                            continue;
                        }
                        ++i;
                    }
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
                                std::move(first),
                                config.save_root,
                                config.allow_new_users);
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
