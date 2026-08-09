#pragma once

#include "common/serialization.hpp"

#include <string_view>

namespace archstreamer {

PairFormRelayAck handle_pair_form_relay_push(const PairFormRelayPush& push);
PairFormRelayResponse handle_pair_form_relay_pull(const PairFormRelayPull& pull);
bool is_pair_form_relay_packet(const PacketPayload& payload);
ByteBuffer handle_pair_form_relay_packet(const PacketPayload& payload);

} // namespace archstreamer
