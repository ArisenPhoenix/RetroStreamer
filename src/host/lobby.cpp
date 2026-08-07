#include "host/lobby.hpp"

#include "host/controls_db_sync.hpp"
#include "host/emulator_orphan_reaper.hpp"
#include "host/host_session_helpers.hpp"
#include "host/launch_environment.hpp"
#include "host/save_active_sessions.hpp"
#include "host/session_lobby.hpp"
#include "host/user_credentials.hpp"
#include "common/serialization.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace archstreamer {

Lobby::Lobby(Config config)
    : config_(std::move(config)) {
    if (!config_.host_config.control_port.has_value()) {
        throw std::runtime_error("Lobby requires --control-port");
    }
    if (config_.catalog == nullptr) {
        throw std::runtime_error("Lobby requires GameCatalog");
    }
    if (config_.streaming_audio == nullptr) {
        throw std::runtime_error("Lobby requires StreamingAudioSink");
    }
    if (!config_.host_config.input_port.has_value()) {
        config_.host_config.input_port = 45454;
    }
    art_root_ = config_.host_config.art_root.empty()
        ? (config_.host_config.rom_root.parent_path() / "Art")
        : config_.host_config.art_root;

    SessionManager::Config session_cfg;
    session_cfg.host_config = config_.host_config;
    session_cfg.catalog = config_.catalog;
    session_cfg.game_list = config_.game_list;
    session_cfg.hub = &hub_;
    session_cfg.input_demux = &demux_;
    session_cfg.streaming_audio = config_.streaming_audio;
    session_cfg.bridge_device = config_.bridge_device;
    session_cfg.should_stop = config_.should_stop;
    session_cfg.max_slots = max_slots();
    session_manager_ = std::make_unique<SessionManager>(std::move(session_cfg));
}

Lobby::~Lobby() {
    request_stop();
    join();
}

std::uint8_t Lobby::max_slots() const {
    return clamp_max_session_slots(config_.host_config.clients);
}

void Lobby::prepare_runtime() {
    const auto slots = max_slots();
    config_.streaming_audio->prune_unused(static_cast<int>(slots), /*keep_legacy=*/false);
    if (!other_host_runner_alive()) {
        std::error_code ec;
        const auto active_dir = active_save_sessions_directory(config_.host_config.save_root);
        if (std::filesystem::is_directory(active_dir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(active_dir, ec)) {
                const auto name = entry.path().filename().string();
                if (name.rfind("slot-", 0) == 0 && entry.path().extension() == ".json") {
                    std::filesystem::remove(entry.path(), ec);
                } else if (name.rfind("connected-", 0) == 0) {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }
    } else {
        std::cout
            << "Keeping shared .archstreamer_active markers "
               "(another host_runner is already running).\n";
    }

    std::cout
        << "Lobby on TCP " << *config_.host_config.control_port
        << " (max slots " << static_cast<int>(slots)
        << ", UDP input " << *config_.host_config.input_port << ").\n"
        << "Buckets: connected / multiplayer / sessions via SessionManager.\n"
        << "Links owned by Lobby (HostSessionHub); sessions tracked by session_id.\n";

    network_receiver_ = std::make_unique<NetworkInputReceiver>(
        *config_.host_config.input_port,
        demux_);
    network_receiver_->start();
    listener_ = std::make_unique<TcpListener>(*config_.host_config.control_port);
}

void Lobby::shutdown_runtime() {
    if (session_manager_ != nullptr) {
        session_manager_->request_stop_all();
        session_manager_->join_all();
    }
    if (network_receiver_ != nullptr) {
        network_receiver_->stop();
        network_receiver_.reset();
    }
    listener_.reset();
    if (config_.streaming_audio != nullptr) {
        config_.streaming_audio->restore_default_sink();
    }
    cleanup_x11_capture_runtime_dir();
}

void Lobby::start() {
    if (started_.exchange(true)) {
        return;
    }
    stop_requested_.store(false);
    prepare_runtime();
    worker_ = std::thread([this] { thread_main(); });
}

void Lobby::request_stop() {
    stop_requested_.store(true);
}

void Lobby::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (started_.exchange(false)) {
        shutdown_runtime();
        std::cout << "Host stopped.\n";
    }
}

void Lobby::enqueue_command(LobbyCommand command) {
    std::lock_guard lock(mutex_);
    pending_commands_.push_back(std::move(command));
}

std::vector<LobbyCommand> Lobby::drain_commands() {
    std::lock_guard lock(mutex_);
    std::vector<LobbyCommand> out;
    out.swap(pending_commands_);
    return out;
}

void Lobby::apply_to_sessions() {
    auto commands = drain_commands();
    if (!commands.empty()) {
        session_manager_->apply_commands(std::move(commands));
    }
    session_manager_->reap_finished();
}

int Lobby::run_until_stop() {
    start();
    while (!(config_.should_stop && config_.should_stop()) && !stop_requested_.load()) {
        apply_to_sessions();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    request_stop();
    join();
    return 0;
}

LobbyStatusSnapshot Lobby::status_snapshot() const {
    LobbyStatusSnapshot snap;
    {
        std::lock_guard lock(mutex_);
        for (const auto& client : connected_) {
            LobbyClientSnapshot row;
            row.client_id = client.client_id;
            row.username = client.username;
            row.bucket = LobbyClientBucket::Connected;
            snap.connected.push_back(std::move(row));
        }
        for (const auto& client : multiplayer_) {
            LobbyClientSnapshot row;
            row.client_id = client.client_id;
            row.username = client.username;
            row.bucket = LobbyClientBucket::Multiplayer;
            row.session_id = client.session_id;
            row.reconnect_deadline = client.reconnect_deadline;
            snap.multiplayer.push_back(std::move(row));
        }
    }
    snap.sessions = session_manager_->statuses();
    for (const auto& session : snap.sessions) {
        if (session.mode != GameSessionMode::SinglePlayer) {
            continue;
        }
        LobbyClientSnapshot row;
        row.bucket = LobbyClientBucket::SinglePlayerSession;
        row.session_id = session.session_id;
        row.username = session.save_username;
        snap.singleplayer_sessions.push_back(std::move(row));
    }
    if (hub_.link_cable().active()) {
        snap.active_link_count = 1;
    }
    return snap;
}

void Lobby::publish_connected(const ConnectedClient& client) const {
    ConnectedClientPresence presence;
    presence.username = client.username;
    presence.client_id = client.client_id;
    presence.slot_index = -1;
    presence.phase = "catalog";
    presence.seated = false;
    publish_connected_client(config_.host_config.save_root, presence);
}

void Lobby::erase_connected_at(std::size_t index) {
    if (index >= connected_.size()) {
        return;
    }
    clear_connected_client(
        config_.host_config.save_root,
        connected_[index].client_id,
        -1);
    connected_.erase(connected_.begin() + static_cast<std::ptrdiff_t>(index));
}

void Lobby::poll_connected_bucket() {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < connected_.size();) {
        auto& client = connected_[i];
        if (auto reason = take_connected_client_disconnect_request(
                config_.host_config.save_root, client.client_id, -1);
            reason.has_value()) {
            std::cerr
                << "Lobby connected " << static_cast<int>(client.client_id)
                << " (" << client.username << ") kicked: " << *reason << '\n';
            try {
                client.stream = TcpStream{};
            } catch (const std::exception&) {
            }
            erase_connected_at(i);
            continue;
        }
        if (!client.stream.open() || client.stream.peer_closed()) {
            std::cout
                << "Lobby connected " << static_cast<int>(client.client_id)
                << " (" << client.username << ") disconnected.\n";
            erase_connected_at(i);
            continue;
        }
        publish_connected(client);
        try {
            while (client.stream.readable()) {
                const auto packet = client.stream.receive_packet();
                if (!packet.has_value()) {
                    erase_connected_at(i);
                    goto next_connected;
                }
                auto payload = deserialize_packet(*packet);
                if (std::holds_alternative<ControlsDbPull>(payload)
                    || std::holds_alternative<ControlsDbPush>(payload)) {
                    auto reply = handle_controls_db_packet(
                        config_.host_config.save_root, client.username, payload);
                    if (!reply.empty()) {
                        client.stream.send_packet(reply);
                    }
                }
            }
        } catch (const std::exception&) {
            erase_connected_at(i);
            continue;
        }
        ++i;
    next_connected:
        continue;
    }
}

void Lobby::handle_presence(TcpStream stream, LobbyPresence presence) {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < connected_.size();) {
        if (connected_[i].username == presence.username) {
            erase_connected_at(i);
            continue;
        }
        ++i;
    }
    ConnectedClient held;
    held.client_id = hub_.allocate_client_id();
    held.username = presence.username;
    held.stream = std::move(stream);
    try {
        held.stream.send_packet(serialize_packet(LobbyPresenceAck{held.client_id}));
    } catch (const std::exception& error) {
        std::cerr
            << "Failed to ack lobby presence for " << held.username
            << ": " << error.what() << '\n';
        return;
    }
    std::cout
        << "Lobby connected " << static_cast<int>(held.client_id)
        << " username=" << held.username << " (catalog)\n";
    publish_connected(held);
    connected_.push_back(std::move(held));
}

void Lobby::handle_hello(TcpStream stream, ClientHello hello) {
    {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < connected_.size();) {
            if (connected_[i].username == hello.username) {
                erase_connected_at(i);
                continue;
            }
            ++i;
        }
    }

    bool handed_off = false;
    try {
        if (hello.requested_players > 0) {
            if (auto* slot = hub_.slot_for_reconnect(hello); slot != nullptr) {
                LobbyCommand command;
                command.kind = LobbyCommand::Kind::EnqueueJoin;
                command.session_id = slot->session_id();
                command.hello = std::move(hello);
                command.stream = std::move(stream);
                command.is_reconnect = true;
                enqueue_command(std::move(command));
                handed_off = true;
                return;
            }
        }

        if (hello.requested_players == 0) {
            if (auto* slot = hub_.slot_for_late_viewer(hello); slot != nullptr) {
                LobbyCommand command;
                command.kind = LobbyCommand::Kind::EnqueueJoin;
                command.session_id = slot->session_id();
                command.hello = std::move(hello);
                command.stream = std::move(stream);
                command.is_reconnect = false;
                enqueue_command(std::move(command));
                handed_off = true;
                return;
            }
            throw std::runtime_error(
                "no active session matches that game for late viewer join");
        }

        if (hello.session_mode == GameSessionMode::Multiplayer) {
            if (session_manager_->live_count() > 0) {
                throw std::runtime_error(
                    "cannot start Multiplayer while singleplayer session slots are active");
            }
            SessionClientConnection first{
                hub_.allocate_client_id(),
                hello,
                std::move(stream),
            };
            {
                std::lock_guard lock(mutex_);
                MultiplayerClient waiting;
                waiting.client_id = first.client_id;
                waiting.username = first.hello.username;
                waiting.hello = first.hello;
                multiplayer_.push_back(std::move(waiting));
            }
            handed_off = true;
            auto plan = gather_session_clients(
                *listener_,
                max_slots(),
                config_.game_list,
                std::chrono::seconds(config_.host_config.session_timeout_seconds),
                std::nullopt,
                [this] {
                    return stop_requested_.load() ||
                        (config_.should_stop && config_.should_stop());
                },
                art_root_,
                std::move(first),
                config_.host_config.save_root,
                config_.host_config.allow_new_users);
            {
                std::lock_guard lock(mutex_);
                multiplayer_.clear();
            }
            LobbyCommand command;
            command.kind = LobbyCommand::Kind::StartSession;
            command.session_id = SessionManager::make_session_id();
            command.plan = std::move(plan);
            enqueue_command(std::move(command));
            return;
        }

        if (hub_.has_multiplayer_slot()) {
            throw std::runtime_error(
                "cannot start Singleplayer while a Multiplayer session is active");
        }
        if (session_manager_->live_count() >= max_slots()) {
            throw std::runtime_error(
                "host is at max concurrent singleplayer sessions "
                "(raise Max clients / --clients)");
        }

        auto plan = make_singleplayer_session_plan(
            hub_.allocate_client_id(),
            std::move(hello),
            std::move(stream),
            config_.game_list,
            config_.host_config.save_root);
        handed_off = true;
        LobbyCommand command;
        command.kind = LobbyCommand::Kind::StartSession;
        command.session_id = SessionManager::make_session_id();
        command.plan = std::move(plan);
        enqueue_command(std::move(command));
    } catch (const std::exception& error) {
        if (!handed_off) {
            try {
                stream.send_packet(serialize_packet(ErrorPacket{error.what()}));
            } catch (const std::exception&) {
            }
        }
        {
            std::lock_guard lock(mutex_);
            multiplayer_.clear();
        }
        std::cerr << "Rejected session start: " << error.what() << '\n';
    }
}

void Lobby::accept_once() {
    auto accepted = try_accept_control_hello(
        *listener_,
        config_.game_list,
        art_root_,
        [this] {
            ActiveSessionInfo info;
            const auto live = session_manager_->live_count();
            info.active = live > 0;
            info.video_enabled = config_.host_config.video;
            info.audio_enabled = config_.host_config.audio;
            info.active_slots = static_cast<std::uint8_t>(live);
            info.max_slots = max_slots();
            if (info.active) {
                for (const auto& session : session_manager_->statuses()) {
                    if (session.finished) {
                        continue;
                    }
                    info.selected_game_id = session.game_id;
                    info.session_mode = session.mode;
                    info.player_count = session.seated_players;
                    break;
                }
            }
            return info;
        },
        config_.host_config.save_root,
        config_.host_config.allow_new_users);

    if (!accepted.has_value()) {
        return;
    }
    if (accepted->have_presence) {
        handle_presence(std::move(accepted->stream), std::move(accepted->presence));
        return;
    }
    if (accepted->have_hello) {
        handle_hello(std::move(accepted->stream), std::move(accepted->hello));
    }
}

void Lobby::thread_main() {
    while (!stop_requested_.load() &&
           !(config_.should_stop && config_.should_stop())) {
        try {
            poll_connected_bucket();
            accept_once();
        } catch (const std::exception& error) {
            if (stop_requested_.load() ||
                (config_.should_stop && config_.should_stop())) {
                break;
            }
            std::cerr << "Host lobby error (staying up): " << error.what() << '\n';
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace archstreamer
