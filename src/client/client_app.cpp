#include "client/client_app.hpp"

#include "client/controller_backend.hpp"
#include "client/gstreamer_media_receiver.hpp"
#include "client/gstreamer_synced_media_session.hpp"
#include "client/media_receiver.hpp"
#include "client/input_sender.hpp"
#include "client/session_service.hpp"
#include "common/addresses.hpp"
#include "common/platform/default_platform.hpp"
#include "common/serialization.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace archstreamer {
namespace {

bool is_number(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const auto character : value) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

// Thin facade so the session loop can use either legacy or synced receivers.
class ActiveMediaPlayback {
public:
    void connect_legacy(const MediaEndpoint& endpoint) {
        synced_.reset();
        legacy_ = std::make_unique<GStreamerMediaReceiver>();
        legacy_->connect(endpoint);
    }

    void connect_synced(const MediaEndpoint& endpoint) {
        legacy_.reset();
        synced_ = std::make_unique<GStreamerSyncedMediaReceiver>();
        synced_->connect(endpoint);
    }

    void disconnect() {
        if (legacy_) {
            legacy_->disconnect();
        }
        if (synced_) {
            synced_->disconnect();
        }
        legacy_.reset();
        synced_.reset();
    }

    bool video_running() const {
        if (synced_) {
            return synced_->video_running();
        }
        return legacy_ && legacy_->video_running();
    }

    bool audio_running() const {
        if (synced_) {
            return synced_->audio_running();
        }
        return legacy_ && legacy_->audio_running();
    }

    bool video_frames_seen() const {
        if (synced_) {
            return synced_->video_frames_seen();
        }
        return legacy_ && legacy_->video_frames_seen();
    }

    std::uint64_t decoded_frame_count() const {
        if (synced_) {
            return synced_->decoded_frame_count();
        }
        return legacy_ ? legacy_->decoded_frame_count() : 0;
    }

    const std::string& video_pipeline_info() const {
        if (synced_) {
            return synced_->video_pipeline_info();
        }
        static const std::string empty;
        return legacy_ ? legacy_->video_pipeline_info() : empty;
    }

    const std::string& audio_pipeline_info() const {
        if (synced_) {
            return synced_->audio_pipeline_info();
        }
        static const std::string empty;
        return legacy_ ? legacy_->audio_pipeline_info() : empty;
    }

    explicit operator bool() const {
        return legacy_ != nullptr || synced_ != nullptr;
    }

private:
    std::unique_ptr<GStreamerMediaReceiver> legacy_;
    std::unique_ptr<GStreamerSyncedMediaReceiver> synced_;
};

bool handle_control_message(TcpStream& stream, const ClientAppCallbacks& callbacks, ClientRunResult& result) {
    if (!stream.readable()) {
        return true;
    }

    const auto payload = receive_client_control_payload(stream);
    if (const auto* ended = std::get_if<SessionEnded>(&payload); ended != nullptr) {
        result.ended_reason = ended->reason;
        if (callbacks.on_session_ended) {
            callbacks.on_session_ended(ended->reason);
        }
        return false;
    }
    if (const auto* error = std::get_if<ErrorPacket>(&payload); error != nullptr) {
        throw std::runtime_error("host ended session: " + error->message);
    }

    return true;
}

std::vector<std::string> selected_device_ids(
    const std::vector<ControllerDevice>& devices,
    const ClientAppConfig& config) {
    std::vector<std::string> ids;
    ids.reserve(config.filter.requested_players);
    for (LocalPlayerIndex player = 0; player < config.filter.requested_players; ++player) {
        const auto device_index = config.controller_indexes[player];
        if (device_index >= devices.size()) {
            throw std::runtime_error("controller index is out of range");
        }
        ids.push_back(devices[device_index].id);
    }
    return ids;
}

std::vector<ControllerInfo> controller_info_for_selection(
    const std::vector<ControllerDevice>& devices,
    const ClientAppConfig& config) {
    std::vector<ControllerInfo> controllers;
    controllers.reserve(config.filter.requested_players);
    for (LocalPlayerIndex player = 0; player < config.filter.requested_players; ++player) {
        const auto device_index = config.controller_indexes[player];
        if (device_index >= devices.size()) {
            throw std::runtime_error("controller index is out of range");
        }

        controllers.push_back(ControllerInfo{
            player,
            devices[device_index].name,
            devices[device_index].guid,
            devices[device_index].vendor_id,
            devices[device_index].product_id,
        });
    }
    return controllers;
}

} // namespace

std::optional<GameId> select_game_id(const GameList& list, const std::optional<std::string>& selector) {
    if (!selector.has_value()) {
        if (list.games.empty()) {
            return std::nullopt;
        }
        return list.games.front().id;
    }
    if (selector->empty()) {
        return std::nullopt;
    }
    if (is_number(*selector)) {
        const auto index = static_cast<std::size_t>(std::stoul(*selector));
        if (index >= list.games.size()) {
            throw std::runtime_error("game index is out of range");
        }
        return list.games[index].id;
    }

    for (const auto& game : list.games) {
        if (game.id == *selector) {
            return game.id;
        }
    }
    throw std::runtime_error("game id was not found in the host game list");
}

bool contains_game_id(const GameList& list, const GameId& game_id) {
    for (const auto& game : list.games) {
        if (game.id == game_id) {
            return true;
        }
    }

    return false;
}

std::vector<ControllerDevice> ClientApp::list_controllers() const {
    ControllerBackend backend;
    return backend.list_devices();
}

ActiveSessionInfo ClientApp::active_session_info(const std::string& host, std::uint16_t control_port) const {
    return ClientSessionService(host, control_port).active_session_info();
}

ClientCatalogView ClientApp::fetch_catalog(
    const ClientAppConfig& config,
    const ClientAppCallbacks& callbacks) const {
    ClientSessionService session_service(config.host, config.control_port);
    auto pending_session = session_service.begin();
    auto filtered_catalog = filter_games(pending_session.game_list, config.filter);
    if (callbacks.on_catalog) {
        callbacks.on_catalog(pending_session.game_list, filtered_catalog);
    }

    return ClientCatalogView{
        std::move(pending_session.game_list),
        std::move(filtered_catalog),
        std::move(pending_session.art_cache_root),
    };
}

ClientSessionDraft ClientApp::begin_session(
    const ClientAppConfig& config,
    const ClientAppCallbacks& callbacks) const {
    ClientSessionService session_service(config.host, config.control_port);
    auto pending_session = session_service.begin();
    auto filtered_catalog = filter_games(pending_session.game_list, config.filter);
    if (callbacks.on_catalog) {
        callbacks.on_catalog(pending_session.game_list, filtered_catalog);
    }

    return ClientSessionDraft{
        std::move(pending_session),
        std::move(filtered_catalog),
    };
}

ClientRunResult ClientApp::join_session(
    ClientSessionDraft draft,
    const ClientAppConfig& config,
    const std::function<bool()>& should_stop,
    const ClientAppCallbacks& callbacks) const {
    if (!valid_username(config.username)) {
        throw std::runtime_error("username must be 1-64 characters and contain only letters, numbers, underscores, or hyphens");
    }
    if (!valid_player_count(config.filter.requested_players)) {
        throw std::runtime_error("requested players must be 0, 1, or 2");
    }
    if (config.role == ClientParticipantRole::Viewer && config.filter.requested_players != 0) {
        throw std::runtime_error("viewer clients cannot request player seats");
    }
    if (config.controller_indexes.size() < config.filter.requested_players) {
        throw std::runtime_error("not enough selected controllers for requested players");
    }
    if (config.controller_indexes.size() > MaxPlayersPerClient) {
        throw std::runtime_error("client can select at most two controllers");
    }

    ClientRunResult result;
    result.full_catalog = draft.pending_session.game_list;
    result.filtered_catalog = draft.filtered_catalog;

    result.selected_game_id = select_game_id(result.filtered_catalog, config.game_selector);
    if (result.selected_game_id.has_value() && !contains_game_id(result.full_catalog, *result.selected_game_id)) {
        throw std::runtime_error("selected game is not in the host game list");
    }
    if (!result.selected_game_id.has_value() && result.filtered_catalog.games.empty()) {
        throw std::runtime_error("no games match the selected filters");
    }

    auto controller_device_ids = std::vector<std::string>{};
    auto controllers = std::vector<ControllerInfo>{};
    if (config.filter.requested_players > 0) {
        ControllerBackend backend;
        const auto devices = backend.list_devices();
        controller_device_ids = selected_device_ids(devices, config);
        controllers = controller_info_for_selection(devices, config);
    }

    const auto hello = draft.pending_session.session.make_hello(
        config.username,
        config.display_name.empty() ? config.username : config.display_name,
        result.selected_game_id,
        config.session_mode,
        config.filter.requested_players,
        std::move(controllers),
        config.wants_video,
        config.wants_audio);
    ClientSessionService session_service(config.host, config.control_port);
    auto joined_session = session_service.finish_join(std::move(draft.pending_session), hello);
    auto& session = joined_session.session;

    result.client_id = session.client_id();
    result.seats = session.seats();
    result.ready = joined_session.ready;
    if (callbacks.on_connected && result.client_id.has_value()) {
        callbacks.on_connected(ClientConnectionInfo{
            *result.client_id,
            config.username,
            config.role,
            config.session_mode,
            result.selected_game_id,
        });
    }
    if (callbacks.on_seat_assignment) {
        callbacks.on_seat_assignment(result.seats);
    }
    if (callbacks.on_session_ready) {
        callbacks.on_session_ready(result.ready);
    }

    const auto start = session_service.wait_for_starting(joined_session.stream);
    result.starting = start.starting;
    result.media_endpoint = start.media_endpoint;

    auto media_receiver = ActiveMediaPlayback{};
    const bool expect_video =
        config.wants_video &&
        result.media_endpoint.has_value() &&
        !result.media_endpoint->video_uri.empty();
    const bool expect_audio =
        config.wants_audio &&
        result.media_endpoint.has_value() &&
        !result.media_endpoint->audio_uri.empty();
    if (expect_video || expect_audio) {
        if (callbacks.on_media_endpoint) {
            callbacks.on_media_endpoint(*result.media_endpoint);
        }
        const auto endpoint = MediaEndpoint{
            expect_video ? result.media_endpoint->video_uri : "",
            expect_audio ? result.media_endpoint->audio_uri : "",
        };
        if (config.synced_av) {
            media_receiver.connect_synced(endpoint);
            if (callbacks.on_status) {
                callbacks.on_status("Using synced A/V pipeline (shared GStreamer clock).");
            }
        } else {
            media_receiver.connect_legacy(endpoint);
        }
        if (callbacks.on_status) {
            if (!media_receiver.video_pipeline_info().empty()) {
                callbacks.on_status("Video pipeline: " + media_receiver.video_pipeline_info());
            }
            if (!media_receiver.audio_pipeline_info().empty()) {
                callbacks.on_status("Audio pipeline: " + media_receiver.audio_pipeline_info());
            }
            if (expect_video) {
                const auto port = video_port_from_endpoint(*result.media_endpoint);
                callbacks.on_status(
                    "If no video window appears, open UDP " + std::to_string(port) +
                    "+ on THIS client (firewalld/ufw). Control/input can work while media is blocked.");
            }
        }
    }
    if (callbacks.on_session_starting) {
        callbacks.on_session_starting(result.starting);
    }

    auto controller_backend = std::optional<ControllerBackend>{};
    auto input_sender = std::optional<InputSender>{};
    auto input_socket = std::optional<UdpSocket>{};
    if (config.input_port.has_value() && config.filter.requested_players > 0 && result.client_id.has_value()) {
        controller_backend.emplace();
        controller_backend->open_selected(controller_device_ids);
        input_sender.emplace(*result.client_id);
        input_socket.emplace();
        if (callbacks.on_input_streaming_started) {
            callbacks.on_input_streaming_started(config.host, *config.input_port);
        }
    } else if (callbacks.on_waiting_without_input) {
        callbacks.on_waiting_without_input();
    }

    std::uint32_t heartbeat_sequence = 0;
    std::uint64_t last_decoded_frames = 0;
    bool video_was_running = false;
    auto next_heartbeat = std::chrono::steady_clock::now();
    auto media_watch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto media_watch_armed = static_cast<bool>(media_receiver);
    while (!should_stop()) {
        if (!handle_control_message(joined_session.stream, callbacks, result)) {
            break;
        }
        if (joined_session.stream.peer_closed()) {
            result.host_disconnected = true;
            if (callbacks.on_host_disconnected) {
                callbacks.on_host_disconnected();
            }
            break;
        }

        if (expect_video && media_receiver) {
            if (media_receiver.video_running()) {
                video_was_running = true;
            } else if (video_was_running) {
                // Closing the sink window (or a mid-session pipeline crash) stops gst-launch.
                result.ended_reason = "video window closed";
                if (callbacks.on_session_ended) {
                    callbacks.on_session_ended(*result.ended_reason);
                }
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (media_watch_armed && now >= media_watch_deadline) {
            media_watch_armed = false;
            if (expect_video && media_receiver && !media_receiver.video_running()) {
                if (callbacks.on_status) {
                    callbacks.on_status("Video receiver died — check GStreamer plugins / display sink.");
                }
            } else if (expect_audio && media_receiver && !media_receiver.audio_running()) {
                if (callbacks.on_status) {
                    callbacks.on_status("Audio receiver died — check GStreamer Opus/Pulse plugins.");
                }
            } else if (expect_video && media_receiver && !media_receiver.video_frames_seen()) {
                if (callbacks.on_status) {
                    callbacks.on_status(
                        "Video receiver is up but no decoded frames yet. "
                        "Usually RTP video datagrams are dropped on Wi‑Fi/VPN (host now uses mtu=1200), "
                        "or the host capture display has no picture. See gst-video-receiver.log in cache.");
                }
            } else if (callbacks.on_status) {
                callbacks.on_status("Media receivers still running; decoded video frames are flowing.");
            }
        }
        if (now >= next_heartbeat && result.client_id.has_value()) {
            std::uint16_t frames_delta = 0;
            std::uint16_t loss_permille = 0;
            if (expect_video && media_receiver) {
                const auto frames = media_receiver.decoded_frame_count();
                const auto delta = frames >= last_decoded_frames ? frames - last_decoded_frames : 0;
                last_decoded_frames = frames;
                frames_delta = static_cast<std::uint16_t>(std::min<std::uint64_t>(delta, 65535));
                if (!media_receiver.video_running()) {
                    loss_permille = 1000;
                } else if (frames_delta == 0) {
                    // No decoded frames in the last second — treat as heavy loss until RTP stats exist.
                    loss_permille = 500;
                }
            }
            joined_session.stream.send_packet(serialize_packet(ViewerHeartbeat{
                *result.client_id,
                heartbeat_sequence++,
                loss_permille,
                frames_delta,
                config.wanted_tier,
                config.max_bitrate_kbps,
            }));
            next_heartbeat = now + std::chrono::seconds(1);
        }

        if (controller_backend.has_value() && input_sender.has_value() && input_socket.has_value()) {
            for (LocalPlayerIndex player = 0; player < config.filter.requested_players; ++player) {
                const auto state = controller_backend->poll(player);
                if (state.has_value()) {
                    const auto packet = input_sender->make_input(player, *state);
                    input_socket->send_to(serialize_packet(packet), config.host, *config.input_port);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (media_receiver) {
        media_receiver.disconnect();
    }

    return result;
}

ClientRunResult ClientApp::run_session(
    const ClientAppConfig& config,
    const std::function<bool()>& should_stop,
    const ClientAppCallbacks& callbacks) const {
    auto draft = begin_session(config, callbacks);
    return join_session(std::move(draft), config, should_stop, callbacks);
}

} // namespace archstreamer
