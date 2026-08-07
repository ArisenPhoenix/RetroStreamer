#pragma once

#include "archstreamer/runtime_cadence/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace archstreamer::cadence {

/** Wire protocol: uint32 big-endian length + UTF-8 JSON object. */
constexpr std::uint32_t kMaxProtocolPayload = 1u << 20; // 1 MiB

nlohmann::json user_to_json(const UserRecord& user);
UserRecord user_from_json(const nlohmann::json& j);

nlohmann::json controls_to_json(const ControlsRecord& controls);
ControlsRecord controls_from_json(const nlohmann::json& j);

nlohmann::json session_to_json(const SessionRecord& session);
SessionRecord session_from_json(const nlohmann::json& j);

nlohmann::json connection_to_json(const ConnectionRecord& connection);
ConnectionRecord connection_from_json(const nlohmann::json& j);

nlohmann::json claim_to_json(const ResourceClaim& claim);
ResourceClaim claim_from_json(const nlohmann::json& j);

nlohmann::json event_to_json(const RuntimeEvent& event);
RuntimeEvent event_from_json(const nlohmann::json& j);

/** Build a request envelope: {"op":..., ...fields}. */
nlohmann::json make_request(const std::string& op, nlohmann::json fields = nlohmann::json::object());

bool read_frame(int fd, std::string& out_payload);
bool write_frame(int fd, const std::string& payload);

} // namespace archstreamer::cadence
