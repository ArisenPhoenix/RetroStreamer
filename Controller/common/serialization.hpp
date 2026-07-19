#pragma once

#include "protocol.hpp"

#include <climits>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace archstreamer {

using ByteBuffer = std::vector<std::uint8_t>;

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

    const ByteBuffer& bytes() const { return bytes_; }
    ByteBuffer take() { return std::move(bytes_); }

private:
    ByteBuffer bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    template <typename T>
    T read_pod() {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
        using Raw = WireRaw<T>;
        using UnsignedRaw = std::make_unsigned_t<Raw>;
        require(sizeof(Raw));

        UnsignedRaw raw = 0;
        for (std::size_t i = 0; i < sizeof(Raw); ++i) {
            raw |= static_cast<UnsignedRaw>(bytes_[offset_++]) << (i * 8);
        }
        return static_cast<T>(static_cast<Raw>(raw));
    }

    bool read_bool() {
        return read_pod<std::uint8_t>() != 0;
    }

    std::string read_string() {
        const auto size = read_pod<std::uint16_t>();
        require(size);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return value;
    }

    bool exhausted() const { return offset_ == bytes_.size(); }

private:
    void require(std::size_t count) const {
        if (offset_ + count > bytes_.size()) {
            throw std::runtime_error("truncated protocol packet");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

inline ClientRole role_for_player_count(std::uint8_t requested_players) {
    return requested_players == 0 ? ClientRole::Viewer : ClientRole::Player;
}

inline bool valid_player_count(std::uint8_t requested_players) {
    return requested_players <= MaxPlayersPerClient;
}

inline void write_controller_state(Writer& writer, const ControllerState& state) {
    writer.write_pod<std::uint32_t>(state.sequence);
    writer.write_pod<std::uint32_t>(state.buttons);
    writer.write_pod<std::int16_t>(state.left_x);
    writer.write_pod<std::int16_t>(state.left_y);
    writer.write_pod<std::int16_t>(state.right_x);
    writer.write_pod<std::int16_t>(state.right_y);
    writer.write_pod<std::uint16_t>(state.left_trigger);
    writer.write_pod<std::uint16_t>(state.right_trigger);
}

inline ControllerState read_controller_state(Reader& reader) {
    ControllerState state;
    state.sequence = reader.read_pod<std::uint32_t>();
    state.buttons = reader.read_pod<std::uint32_t>();
    state.left_x = reader.read_pod<std::int16_t>();
    state.left_y = reader.read_pod<std::int16_t>();
    state.right_x = reader.read_pod<std::int16_t>();
    state.right_y = reader.read_pod<std::int16_t>();
    state.left_trigger = reader.read_pod<std::uint16_t>();
    state.right_trigger = reader.read_pod<std::uint16_t>();
    return state;
}

inline ByteBuffer serialize_payload(const ClientHello& payload) {
    Writer writer;
    writer.write_string(payload.display_name);
    writer.write_pod<std::uint8_t>(payload.requested_players);
    writer.write_bool(payload.wants_video);
    writer.write_bool(payload.wants_audio);
    return writer.take();
}

inline ByteBuffer serialize_payload(const HostWelcome& payload) {
    Writer writer;
    writer.write_pod<ClientId>(payload.client_id);
    writer.write_pod<std::uint8_t>(payload.max_players_for_client);
    writer.write_bool(payload.host_is_player);
    return writer.take();
}

inline ByteBuffer serialize_payload(const ClientConfig& payload) {
    Writer writer;
    writer.write_pod<std::uint8_t>(payload.requested_players);
    writer.write_bool(payload.wants_video);
    writer.write_bool(payload.wants_audio);
    return writer.take();
}

inline ByteBuffer serialize_payload(const SeatAssignment& payload) {
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

inline ByteBuffer serialize_payload(const ControllerInput& payload) {
    Writer writer;
    writer.write_pod<ClientId>(payload.client_id);
    writer.write_pod<LocalPlayerIndex>(payload.local_player);
    write_controller_state(writer, payload.state);
    return writer.take();
}

inline ByteBuffer serialize_payload(const ViewerHeartbeat& payload) {
    Writer writer;
    writer.write_pod<ClientId>(payload.client_id);
    writer.write_pod<std::uint32_t>(payload.sequence);
    return writer.take();
}

inline ByteBuffer serialize_payload(const ErrorPacket& payload) {
    Writer writer;
    writer.write_string(payload.message);
    return writer.take();
}

inline PacketType packet_type_for(const ClientHello&) { return PacketType::ClientHello; }
inline PacketType packet_type_for(const HostWelcome&) { return PacketType::HostWelcome; }
inline PacketType packet_type_for(const ClientConfig&) { return PacketType::ClientConfig; }
inline PacketType packet_type_for(const SeatAssignment&) { return PacketType::SeatAssignment; }
inline PacketType packet_type_for(const ControllerInput&) { return PacketType::ControllerInput; }
inline PacketType packet_type_for(const ViewerHeartbeat&) { return PacketType::ViewerHeartbeat; }
inline PacketType packet_type_for(const ErrorPacket&) { return PacketType::Error; }

template <typename Payload>
ByteBuffer serialize_packet(const Payload& payload) {
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

inline ClientHello read_client_hello(Reader& reader) {
    ClientHello payload;
    payload.display_name = reader.read_string();
    payload.requested_players = reader.read_pod<std::uint8_t>();
    payload.wants_video = reader.read_bool();
    payload.wants_audio = reader.read_bool();
    return payload;
}

inline HostWelcome read_host_welcome(Reader& reader) {
    HostWelcome payload;
    payload.client_id = reader.read_pod<ClientId>();
    payload.max_players_for_client = reader.read_pod<std::uint8_t>();
    payload.host_is_player = reader.read_bool();
    return payload;
}

inline ClientConfig read_client_config(Reader& reader) {
    ClientConfig payload;
    payload.requested_players = reader.read_pod<std::uint8_t>();
    payload.wants_video = reader.read_bool();
    payload.wants_audio = reader.read_bool();
    return payload;
}

inline SeatAssignment read_seat_assignment(Reader& reader) {
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

inline ControllerInput read_controller_input(Reader& reader) {
    ControllerInput payload;
    payload.client_id = reader.read_pod<ClientId>();
    payload.local_player = reader.read_pod<LocalPlayerIndex>();
    payload.state = read_controller_state(reader);
    return payload;
}

inline ViewerHeartbeat read_viewer_heartbeat(Reader& reader) {
    ViewerHeartbeat payload;
    payload.client_id = reader.read_pod<ClientId>();
    payload.sequence = reader.read_pod<std::uint32_t>();
    return payload;
}

inline ErrorPacket read_error_packet(Reader& reader) {
    return ErrorPacket{reader.read_string()};
}

inline PacketPayload deserialize_packet(std::span<const std::uint8_t> packet) {
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
    }

    throw std::runtime_error("unknown packet type");
}

} // namespace archstreamer
