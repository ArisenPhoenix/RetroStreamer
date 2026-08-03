#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace archstreamer {

using ByteBuffer = std::vector<std::uint8_t>;

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes);

    template <typename T>
    T read_pod() {
        require(sizeof(T));

        using Raw = WireRawType<T>::type;
        Raw value = 0;
        for (std::size_t i = 0; i < sizeof(Raw); ++i) {
            value |= static_cast<Raw>(bytes_[offset_ + i]) << (i * 8);
        }
        offset_ += sizeof(Raw);
        return static_cast<T>(value);
    }

    bool read_bool();
    std::string read_string();
    std::optional<std::string> read_optional_string();
    std::vector<std::uint8_t> read_bytes();
    std::size_t remaining() const { return bytes_.size() - offset_; }

private:
    template <typename T, bool IsEnum = std::is_enum_v<T>>
    struct WireRawType {
        using type = T;
    };

    template <typename T>
    struct WireRawType<T, true> {
        using type = std::underlying_type_t<T>;
    };

    void require(std::size_t count) const;

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

ByteBuffer serialize_payload(const ClientHello& payload);
ByteBuffer serialize_payload(const HostWelcome& payload);
ByteBuffer serialize_payload(const ClientConfig& payload);
ByteBuffer serialize_payload(const SeatAssignment& payload);
ByteBuffer serialize_payload(const ControllerInput& payload);
ByteBuffer serialize_payload(const KeyboardInput& payload);
ByteBuffer serialize_payload(const ViewerHeartbeat& payload);
ByteBuffer serialize_payload(const ErrorPacket& payload);
ByteBuffer serialize_payload(const GameListRequest& payload);
ByteBuffer serialize_payload(const GameList& payload);
ByteBuffer serialize_payload(const ActiveSessionInfoRequest& payload);
ByteBuffer serialize_payload(const ActiveSessionInfo& payload);
ByteBuffer serialize_payload(const SessionReady& payload);
ByteBuffer serialize_payload(const SessionStarting& payload);
ByteBuffer serialize_payload(const SessionEnded& payload);
ByteBuffer serialize_payload(const ClientSessionLeave& payload);
ByteBuffer serialize_payload(const MediaEndpoint& payload);
ByteBuffer serialize_payload(const ArtAssetRequest& payload);
ByteBuffer serialize_payload(const ArtAssetResponse& payload);
ByteBuffer serialize_payload(const DiscControlRequest& payload);
ByteBuffer serialize_payload(const DiscControlResponse& payload);
ByteBuffer serialize_payload(const LinkRequest& payload);
ByteBuffer serialize_payload(const LinkResponse& payload);
ByteBuffer serialize_payload(const SoftKeyboardRequest& payload);
ByteBuffer serialize_payload(const SoftKeyboardResponse& payload);
ByteBuffer serialize_payload(const MediaVideoPending& payload);
ByteBuffer serialize_payload(const MediaVideoReady& payload);
ByteBuffer serialize_payload(const EmulatorControl& payload);
ByteBuffer serialize_payload(const ClientLogBundle& payload);
ByteBuffer serialize_payload(const PasswordChangeRequired& payload);
ByteBuffer serialize_payload(const PasswordChange& payload);

PacketType packet_type_for(const ClientHello& payload);
PacketType packet_type_for(const HostWelcome& payload);
PacketType packet_type_for(const ClientConfig& payload);
PacketType packet_type_for(const SeatAssignment& payload);
PacketType packet_type_for(const ControllerInput& payload);
PacketType packet_type_for(const KeyboardInput& payload);
PacketType packet_type_for(const ViewerHeartbeat& payload);
PacketType packet_type_for(const ErrorPacket& payload);
PacketType packet_type_for(const GameListRequest& payload);
PacketType packet_type_for(const GameList& payload);
PacketType packet_type_for(const ActiveSessionInfoRequest& payload);
PacketType packet_type_for(const ActiveSessionInfo& payload);
PacketType packet_type_for(const SessionReady& payload);
PacketType packet_type_for(const SessionStarting& payload);
PacketType packet_type_for(const SessionEnded& payload);
PacketType packet_type_for(const ClientSessionLeave& payload);
PacketType packet_type_for(const MediaEndpoint& payload);
PacketType packet_type_for(const ArtAssetRequest& payload);
PacketType packet_type_for(const ArtAssetResponse& payload);
PacketType packet_type_for(const DiscControlRequest& payload);
PacketType packet_type_for(const DiscControlResponse& payload);
PacketType packet_type_for(const LinkRequest& payload);
PacketType packet_type_for(const LinkResponse& payload);
PacketType packet_type_for(const SoftKeyboardRequest& payload);
PacketType packet_type_for(const SoftKeyboardResponse& payload);
PacketType packet_type_for(const MediaVideoPending& payload);
PacketType packet_type_for(const MediaVideoReady& payload);
PacketType packet_type_for(const EmulatorControl& payload);
PacketType packet_type_for(const ClientLogBundle& payload);
PacketType packet_type_for(const PasswordChangeRequired& payload);
PacketType packet_type_for(const PasswordChange& payload);

ByteBuffer serialize_packet(const ClientHello& payload);
ByteBuffer serialize_packet(const HostWelcome& payload);
ByteBuffer serialize_packet(const ClientConfig& payload);
ByteBuffer serialize_packet(const SeatAssignment& payload);
ByteBuffer serialize_packet(const ControllerInput& payload);
ByteBuffer serialize_packet(const KeyboardInput& payload);
ByteBuffer serialize_packet(const ViewerHeartbeat& payload);
ByteBuffer serialize_packet(const ErrorPacket& payload);
ByteBuffer serialize_packet(const GameListRequest& payload);
ByteBuffer serialize_packet(const GameList& payload);
ByteBuffer serialize_packet(const ActiveSessionInfoRequest& payload);
ByteBuffer serialize_packet(const ActiveSessionInfo& payload);
ByteBuffer serialize_packet(const SessionReady& payload);
ByteBuffer serialize_packet(const SessionStarting& payload);
ByteBuffer serialize_packet(const SessionEnded& payload);
ByteBuffer serialize_packet(const ClientSessionLeave& payload);
ByteBuffer serialize_packet(const MediaEndpoint& payload);
ByteBuffer serialize_packet(const ArtAssetRequest& payload);
ByteBuffer serialize_packet(const ArtAssetResponse& payload);
ByteBuffer serialize_packet(const DiscControlRequest& payload);
ByteBuffer serialize_packet(const DiscControlResponse& payload);
ByteBuffer serialize_packet(const LinkRequest& payload);
ByteBuffer serialize_packet(const LinkResponse& payload);
ByteBuffer serialize_packet(const SoftKeyboardRequest& payload);
ByteBuffer serialize_packet(const SoftKeyboardResponse& payload);
ByteBuffer serialize_packet(const MediaVideoPending& payload);
ByteBuffer serialize_packet(const MediaVideoReady& payload);
ByteBuffer serialize_packet(const EmulatorControl& payload);
ByteBuffer serialize_packet(const ClientLogBundle& payload);
ByteBuffer serialize_packet(const PasswordChangeRequired& payload);
ByteBuffer serialize_packet(const PasswordChange& payload);

PacketPayload deserialize_packet(std::span<const std::uint8_t> packet);

} // namespace archstreamer
