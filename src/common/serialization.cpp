#include "common/serialization.hpp"

#include <climits>
#include <cstddef>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace archstreamer {

namespace {

template <typename T, bool IsEnum = std::is_enum_v<T>>
struct WireRawType {
    using type = T;
};

template <typename T>
struct WireRawType<T, true> {
    using type = std::underlying_type_t<T>;
};

template <typename T>
using WireRaw = typename WireRawType<T>::type;

class Writer {
public:
    template <typename T>
    void write_pod(T value) {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
        using Raw = WireRaw<T>;
        using UnsignedRaw = std::make_unsigned_t<Raw>;
        UnsignedRaw raw = static_cast<UnsignedRaw>(static_cast<Raw>(value));
        for (std::size_t i = 0; i < sizeof(Raw); ++i) {
            bytes_.push_back(static_cast<std::uint8_t>((raw >> (i * 8)) & 0xff));
        }
    }

    void write_bool(bool value) {
        write_pod<std::uint8_t>(value ? 1 : 0);
    }

    void write_string(std::string_view value) {
        if (value.size() > UINT16_MAX) {
            throw std::runtime_error("string too large for protocol packet");
        }
        write_pod<std::uint16_t>(static_cast<std::uint16_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void write_optional_string(const std::optional<std::string>& value) {
        write_bool(value.has_value());
        if (value.has_value()) {
            write_string(*value);
        }
    }

    void write_bytes(std::span<const std::uint8_t> value) {
        if (value.size() > 16 * 1024 * 1024) {
            throw std::runtime_error("binary payload too large for protocol packet");
        }
        write_pod<std::uint32_t>(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    const ByteBuffer& bytes() const { return bytes_; }
    ByteBuffer take() { return std::move(bytes_); }

private:
    ByteBuffer bytes_;
};

} // namespace

Reader::Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

void Reader::require(std::size_t count) const {
    if (offset_ + count > bytes_.size()) {
        throw std::runtime_error("truncated protocol packet");
    }
}

bool Reader::read_bool() {
    return read_pod<std::uint8_t>() != 0;
}

std::string Reader::read_string() {
    const auto size = read_pod<std::uint16_t>();
    require(size);
    std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return value;
}

std::optional<std::string> Reader::read_optional_string() {
    if (!read_bool()) {
        return std::nullopt;
    }
    return read_string();
}

std::vector<std::uint8_t> Reader::read_bytes() {
    const auto size = read_pod<std::uint32_t>();
    require(size);
    std::vector<std::uint8_t> value(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                    bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += size;
    return value;
}

void write_controller_state(Writer& writer, const ControllerState& state) {
    writer.write_pod<std::uint32_t>(state.sequence);
    writer.write_pod<std::uint64_t>(state.timestamp_us);
    writer.write_pod<std::uint32_t>(state.buttons);
    writer.write_pod<std::int16_t>(state.left_x);
    writer.write_pod<std::int16_t>(state.left_y);
    writer.write_pod<std::int16_t>(state.right_x);
    writer.write_pod<std::int16_t>(state.right_y);
    writer.write_pod<std::uint16_t>(state.left_trigger);
    writer.write_pod<std::uint16_t>(state.right_trigger);
}

ControllerState read_controller_state(Reader& reader) {
    ControllerState state;
    state.sequence = reader.read_pod<std::uint32_t>();
    state.timestamp_us = reader.read_pod<std::uint64_t>();
    state.buttons = reader.read_pod<std::uint32_t>();
    state.left_x = reader.read_pod<std::int16_t>();
    state.left_y = reader.read_pod<std::int16_t>();
    state.right_x = reader.read_pod<std::int16_t>();
    state.right_y = reader.read_pod<std::int16_t>();
    state.left_trigger = reader.read_pod<std::uint16_t>();
    state.right_trigger = reader.read_pod<std::uint16_t>();
    return state;
}

void write_controller_info(Writer& writer, const ControllerInfo& controller) {
    writer.write_pod<LocalPlayerIndex>(controller.local_player);
    writer.write_string(controller.name);
    writer.write_string(controller.guid);
    writer.write_pod<std::uint16_t>(controller.vendor_id);
    writer.write_pod<std::uint16_t>(controller.product_id);
}

ControllerInfo read_controller_info(Reader& reader) {
    ControllerInfo controller;
    controller.local_player = reader.read_pod<LocalPlayerIndex>();
    controller.name = reader.read_string();
    controller.guid = reader.read_string();
    controller.vendor_id = reader.read_pod<std::uint16_t>();
    controller.product_id = reader.read_pod<std::uint16_t>();
    return controller;
}

void write_controller_infos(Writer& writer, const std::vector<ControllerInfo>& controllers) {
    if (!valid_controller_info_count(controllers.size())) {
        throw std::runtime_error("too many controller info entries");
    }

    writer.write_pod<std::uint8_t>(static_cast<std::uint8_t>(controllers.size()));
    for (const auto& controller : controllers) {
        write_controller_info(writer, controller);
    }
}

std::vector<ControllerInfo> read_controller_infos(Reader& reader) {
    const auto count = reader.read_pod<std::uint8_t>();
    if (!valid_controller_info_count(count)) {
        throw std::runtime_error("too many controller info entries");
    }

    std::vector<ControllerInfo> controllers;
    controllers.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        controllers.push_back(read_controller_info(reader));
    }
    return controllers;
}

ByteBuffer serialize_payload(const ClientHello& payload) {
    Writer writer;
    writer.write_string(payload.username);
    writer.write_string(payload.display_name);
    writer.write_optional_string(payload.selected_game_id);
    writer.write_pod<GameSessionMode>(payload.session_mode);
    writer.write_pod<std::uint8_t>(payload.requested_players);
    write_controller_infos(writer, payload.controllers);
    writer.write_bool(payload.wants_video);
    writer.write_bool(payload.wants_audio);
    return writer.take();
}

ByteBuffer serialize_payload(const HostWelcome& payload) {
    Writer writer;
    writer.write_pod<ClientId>(payload.client_id);
    writer.write_pod<std::uint8_t>(payload.max_players_for_client);
    writer.write_bool(payload.host_is_player);
    return writer.take();
}

ByteBuffer serialize_payload(const ClientConfig& payload) {
    Writer writer;
    writer.write_optional_string(payload.username);
    writer.write_optional_string(payload.display_name);
    writer.write_optional_string(payload.selected_game_id);
    writer.write_pod<GameSessionMode>(payload.session_mode);
    writer.write_pod<std::uint8_t>(payload.requested_players);
    write_controller_infos(writer, payload.controllers);
    writer.write_bool(payload.wants_video);
    writer.write_bool(payload.wants_audio);
    return writer.take();
}

ByteBuffer serialize_payload(const SeatAssignment& payload) {
    if (payload.seats.size() > UINT8_MAX) {
        throw std::runtime_error("too many seat assignments");
    }

    Writer writer;
    writer.write_pod<std::uint8_t>(static_cast<std::uint8_t>(payload.seats.size()));
    for (const auto& seat : payload.seats) {
        writer.write_pod<ClientId>(seat.client_id);
        writer.write_pod<LocalPlayerIndex>(seat.local_player);
        writer.write_pod<RetroArchPort>(seat.retroarch_port);
    }
    return writer.take();
}

ByteBuffer serialize_payload(const ControllerInput& payload) {
    Writer writer;
    writer.write_pod<ClientId>(payload.client_id);
    writer.write_pod<LocalPlayerIndex>(payload.local_player);
    write_controller_state(writer, payload.state);
    return writer.take();
}

ByteBuffer serialize_payload(const ViewerHeartbeat& payload) {
    Writer writer;
    writer.write_pod<ClientId>(payload.client_id);
    writer.write_pod<std::uint32_t>(payload.sequence);
    writer.write_pod<std::uint16_t>(payload.loss_permille);
    writer.write_pod<std::uint16_t>(payload.frames_decoded_delta);
    writer.write_pod<std::uint8_t>(static_cast<std::uint8_t>(payload.wanted_tier));
    writer.write_pod<std::uint16_t>(payload.max_bitrate_kbps);
    return writer.take();
}

ByteBuffer serialize_payload(const ErrorPacket& payload) {
    Writer writer;
    writer.write_string(payload.message);
    return writer.take();
}

ByteBuffer serialize_payload(const GameListRequest& payload) {
    Writer writer;
    writer.write_pod<std::uint64_t>(payload.client_catalog_revision);
    return writer.take();
}

ByteBuffer serialize_payload(const GameList& payload) {
    if (payload.games.size() > UINT16_MAX) {
        throw std::runtime_error("too many games for protocol packet");
    }
    if (payload.deleted_game_ids.size() > UINT16_MAX) {
        throw std::runtime_error("too many deleted games for protocol packet");
    }

    Writer writer;
    writer.write_pod<std::uint64_t>(payload.catalog_revision);
    writer.write_bool(payload.full);
    writer.write_pod<std::uint16_t>(static_cast<std::uint16_t>(payload.games.size()));
    for (const auto& game : payload.games) {
        writer.write_string(game.id);
        writer.write_string(game.identity_key);
        writer.write_string(game.asset_key);
        writer.write_string(game.display_name);
        writer.write_string(game.system_name);
        writer.write_string(game.system_key);
        writer.write_string(game.core_name);
        writer.write_string(game.canonical_name);
        writer.write_string(game.version);
        writer.write_string(game.language);
        writer.write_string(game.region);
        writer.write_bool(game.supports_singleplayer);
        writer.write_bool(game.supports_multiplayer);
        writer.write_pod<std::uint8_t>(game.min_players);
        writer.write_pod<std::uint8_t>(game.max_players);
        writer.write_pod<std::uint64_t>(game.updated_at);
    }
    writer.write_pod<std::uint16_t>(static_cast<std::uint16_t>(payload.deleted_game_ids.size()));
    for (const auto& game_id : payload.deleted_game_ids) {
        writer.write_string(game_id);
    }
    return writer.take();
}

ByteBuffer serialize_payload(const ActiveSessionInfoRequest&) {
    return {};
}

ByteBuffer serialize_payload(const ActiveSessionInfo& payload) {
    Writer writer;
    writer.write_bool(payload.active);
    writer.write_optional_string(payload.selected_game_id);
    writer.write_pod<GameSessionMode>(payload.session_mode);
    writer.write_pod<std::uint8_t>(payload.player_count);
    writer.write_pod<std::uint8_t>(payload.connected_players);
    writer.write_pod<std::uint8_t>(payload.disconnected_players);
    writer.write_pod<std::uint8_t>(payload.viewer_count);
    writer.write_bool(payload.video_enabled);
    writer.write_bool(payload.audio_enabled);
    return writer.take();
}

ByteBuffer serialize_payload(const SessionReady& payload) {
    Writer writer;
    writer.write_string(payload.selected_game_id);
    writer.write_pod<GameSessionMode>(payload.session_mode);
    writer.write_pod<std::uint8_t>(payload.player_count);
    return writer.take();
}

ByteBuffer serialize_payload(const SessionStarting& payload) {
    Writer writer;
    writer.write_string(payload.selected_game_id);
    writer.write_pod<GameSessionMode>(payload.session_mode);
    writer.write_pod<std::uint8_t>(payload.player_count);
    return writer.take();
}

ByteBuffer serialize_payload(const SessionEnded& payload) {
    Writer writer;
    writer.write_string(payload.reason);
    return writer.take();
}

ByteBuffer serialize_payload(const MediaEndpoint& payload) {
    Writer writer;
    writer.write_string(payload.video_uri);
    writer.write_string(payload.audio_uri);
    return writer.take();
}

ByteBuffer serialize_payload(const ArtAssetRequest& payload) {
    Writer writer;
    writer.write_string(payload.asset_key);
    writer.write_string(payload.role);
    return writer.take();
}

ByteBuffer serialize_payload(const ArtAssetResponse& payload) {
    Writer writer;
    writer.write_string(payload.asset_key);
    writer.write_string(payload.role);
    writer.write_bool(payload.found);
    writer.write_string(payload.extension);
    writer.write_bytes(payload.data);
    return writer.take();
}

PacketType packet_type_for(const ClientHello&) { return PacketType::ClientHello; }
PacketType packet_type_for(const HostWelcome&) { return PacketType::HostWelcome; }
PacketType packet_type_for(const ClientConfig&) { return PacketType::ClientConfig; }
PacketType packet_type_for(const SeatAssignment&) { return PacketType::SeatAssignment; }
PacketType packet_type_for(const ControllerInput&) { return PacketType::ControllerInput; }
PacketType packet_type_for(const ViewerHeartbeat&) { return PacketType::ViewerHeartbeat; }
PacketType packet_type_for(const ErrorPacket&) { return PacketType::Error; }
PacketType packet_type_for(const GameListRequest&) { return PacketType::GameListRequest; }
PacketType packet_type_for(const GameList&) { return PacketType::GameList; }
PacketType packet_type_for(const ActiveSessionInfoRequest&) { return PacketType::ActiveSessionInfoRequest; }
PacketType packet_type_for(const ActiveSessionInfo&) { return PacketType::ActiveSessionInfo; }
PacketType packet_type_for(const SessionReady&) { return PacketType::SessionReady; }
PacketType packet_type_for(const SessionStarting&) { return PacketType::SessionStarting; }
PacketType packet_type_for(const SessionEnded&) { return PacketType::SessionEnded; }
PacketType packet_type_for(const MediaEndpoint&) { return PacketType::MediaEndpoint; }
PacketType packet_type_for(const ArtAssetRequest&) { return PacketType::ArtAssetRequest; }
PacketType packet_type_for(const ArtAssetResponse&) { return PacketType::ArtAssetResponse; }

template <typename Payload>
ByteBuffer serialize_packet_impl(const Payload& payload) {
    ByteBuffer body = serialize_payload(payload);

    Writer writer;
    writer.write_pod<std::uint32_t>(ProtocolMagic);
    writer.write_pod<std::uint16_t>(ProtocolVersion);
    writer.write_pod<PacketType>(packet_type_for(payload));
    writer.write_pod<std::uint32_t>(static_cast<std::uint32_t>(body.size()));

    ByteBuffer packet = writer.take();
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

ByteBuffer serialize_packet(const ClientHello& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const HostWelcome& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ClientConfig& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const SeatAssignment& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ControllerInput& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ViewerHeartbeat& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ErrorPacket& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const GameListRequest& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const GameList& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ActiveSessionInfoRequest& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ActiveSessionInfo& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const SessionReady& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const SessionStarting& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const SessionEnded& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const MediaEndpoint& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ArtAssetRequest& payload) {
    return serialize_packet_impl(payload);
}

ByteBuffer serialize_packet(const ArtAssetResponse& payload) {
    return serialize_packet_impl(payload);
}

ClientHello read_client_hello(Reader& reader) {
    ClientHello payload;
    payload.username = reader.read_string();
    payload.display_name = reader.read_string();
    payload.selected_game_id = reader.read_optional_string();
    payload.session_mode = reader.read_pod<GameSessionMode>();
    payload.requested_players = reader.read_pod<std::uint8_t>();
    payload.controllers = read_controller_infos(reader);
    payload.wants_video = reader.read_bool();
    payload.wants_audio = reader.read_bool();
    return payload;
}

HostWelcome read_host_welcome(Reader& reader) {
    HostWelcome payload;
    payload.client_id = reader.read_pod<ClientId>();
    payload.max_players_for_client = reader.read_pod<std::uint8_t>();
    payload.host_is_player = reader.read_bool();
    return payload;
}

ClientConfig read_client_config(Reader& reader) {
    ClientConfig payload;
    payload.username = reader.read_optional_string();
    payload.display_name = reader.read_optional_string();
    payload.selected_game_id = reader.read_optional_string();
    payload.session_mode = reader.read_pod<GameSessionMode>();
    payload.requested_players = reader.read_pod<std::uint8_t>();
    payload.controllers = read_controller_infos(reader);
    payload.wants_video = reader.read_bool();
    payload.wants_audio = reader.read_bool();
    return payload;
}

SeatAssignment read_seat_assignment(Reader& reader) {
    SeatAssignment payload;
    const auto count = reader.read_pod<std::uint8_t>();
    payload.seats.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        payload.seats.push_back(PlayerSeat{
            reader.read_pod<ClientId>(),
            reader.read_pod<LocalPlayerIndex>(),
            reader.read_pod<RetroArchPort>(),
        });
    }
    return payload;
}

ControllerInput read_controller_input(Reader& reader) {
    ControllerInput payload;
    payload.client_id = reader.read_pod<ClientId>();
    payload.local_player = reader.read_pod<LocalPlayerIndex>();
    payload.state = read_controller_state(reader);
    return payload;
}

ViewerHeartbeat read_viewer_heartbeat(Reader& reader) {
    ViewerHeartbeat payload;
    payload.client_id = reader.read_pod<ClientId>();
    payload.sequence = reader.read_pod<std::uint32_t>();
    payload.loss_permille = reader.read_pod<std::uint16_t>();
    payload.frames_decoded_delta = reader.read_pod<std::uint16_t>();
    payload.wanted_tier = static_cast<MediaQualityTier>(reader.read_pod<std::uint8_t>());
    payload.max_bitrate_kbps = reader.read_pod<std::uint16_t>();
    return payload;
}

ErrorPacket read_error_packet(Reader& reader) {
    return ErrorPacket{reader.read_string()};
}

GameListRequest read_game_list_request(Reader& reader) {
    return GameListRequest{reader.read_pod<std::uint64_t>()};
}

GameList read_game_list(Reader& reader) {
    GameList payload;
    payload.catalog_revision = reader.read_pod<std::uint64_t>();
    payload.full = reader.read_bool();
    const auto count = reader.read_pod<std::uint16_t>();
    payload.games.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        payload.games.push_back(GameInfo{
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_string(),
            reader.read_bool(),
            reader.read_bool(),
            reader.read_pod<std::uint8_t>(),
            reader.read_pod<std::uint8_t>(),
            reader.read_pod<std::uint64_t>(),
        });
    }
    const auto deleted_count = reader.read_pod<std::uint16_t>();
    payload.deleted_game_ids.reserve(deleted_count);
    for (std::uint16_t i = 0; i < deleted_count; ++i) {
        payload.deleted_game_ids.push_back(reader.read_string());
    }
    return payload;
}

ActiveSessionInfoRequest read_active_session_info_request(Reader&) {
    return ActiveSessionInfoRequest{};
}

ActiveSessionInfo read_active_session_info(Reader& reader) {
    return ActiveSessionInfo{
        reader.read_bool(),
        reader.read_optional_string(),
        reader.read_pod<GameSessionMode>(),
        reader.read_pod<std::uint8_t>(),
        reader.read_pod<std::uint8_t>(),
        reader.read_pod<std::uint8_t>(),
        reader.read_pod<std::uint8_t>(),
        reader.read_bool(),
        reader.read_bool(),
    };
}

SessionReady read_session_ready(Reader& reader) {
    return SessionReady{
        reader.read_string(),
        reader.read_pod<GameSessionMode>(),
        reader.read_pod<std::uint8_t>(),
    };
}

SessionStarting read_session_starting(Reader& reader) {
    return SessionStarting{
        reader.read_string(),
        reader.read_pod<GameSessionMode>(),
        reader.read_pod<std::uint8_t>(),
    };
}

SessionEnded read_session_ended(Reader& reader) {
    return SessionEnded{reader.read_string()};
}

MediaEndpoint read_media_endpoint(Reader& reader) {
    return MediaEndpoint{
        reader.read_string(),
        reader.read_string(),
    };
}

ArtAssetRequest read_art_asset_request(Reader& reader) {
    return ArtAssetRequest{reader.read_string(), reader.read_string()};
}

ArtAssetResponse read_art_asset_response(Reader& reader) {
    ArtAssetResponse payload;
    payload.asset_key = reader.read_string();
    payload.role = reader.read_string();
    payload.found = reader.read_bool();
    payload.extension = reader.read_string();
    payload.data = reader.read_bytes();
    return payload;
}

PacketPayload deserialize_packet(std::span<const std::uint8_t> packet) {
    Reader header_reader(packet);
    const auto magic = header_reader.read_pod<std::uint32_t>();
    const auto version = header_reader.read_pod<std::uint16_t>();
    const auto type = header_reader.read_pod<PacketType>();
    const auto payload_size = header_reader.read_pod<std::uint32_t>();

    if (magic != ProtocolMagic) {
        throw std::runtime_error("bad protocol magic");
    }
    if (version != ProtocolVersion) {
        throw std::runtime_error("unsupported protocol version");
    }

    constexpr std::size_t HeaderSize = sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(PacketType) + sizeof(std::uint32_t);
    if (packet.size() != HeaderSize + payload_size) {
        throw std::runtime_error("packet size does not match header");
    }

    Reader payload_reader(packet.subspan(HeaderSize, payload_size));
    switch (type) {
        case PacketType::ClientHello:
            return read_client_hello(payload_reader);
        case PacketType::HostWelcome:
            return read_host_welcome(payload_reader);
        case PacketType::ClientConfig:
            return read_client_config(payload_reader);
        case PacketType::SeatAssignment:
            return read_seat_assignment(payload_reader);
        case PacketType::ControllerInput:
            return read_controller_input(payload_reader);
        case PacketType::ViewerHeartbeat:
            return read_viewer_heartbeat(payload_reader);
        case PacketType::Error:
            return read_error_packet(payload_reader);
        case PacketType::GameListRequest:
            return read_game_list_request(payload_reader);
        case PacketType::GameList:
            return read_game_list(payload_reader);
        case PacketType::ActiveSessionInfoRequest:
            return read_active_session_info_request(payload_reader);
        case PacketType::ActiveSessionInfo:
            return read_active_session_info(payload_reader);
        case PacketType::SessionReady:
            return read_session_ready(payload_reader);
        case PacketType::SessionStarting:
            return read_session_starting(payload_reader);
        case PacketType::SessionEnded:
            return read_session_ended(payload_reader);
        case PacketType::MediaEndpoint:
            return read_media_endpoint(payload_reader);
        case PacketType::ArtAssetRequest:
            return read_art_asset_request(payload_reader);
        case PacketType::ArtAssetResponse:
            return read_art_asset_response(payload_reader);
    }

    throw std::runtime_error("unknown packet type");
}

} // namespace archstreamer
