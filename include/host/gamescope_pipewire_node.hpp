#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Pick a gamescope PipeWire Video/Source node id from a `pw-dump` JSON document.
 *
 * Prefers a node owned by `owner_pid` (OS pid on the PipeWire Client linked by
 * client.id — especially pipewire.sec.pid — or on the node props). Ownership
 * includes process-group peers and descendants of the gamescope wrapper.
 * When owner_pid > 0, never returns an unowned node (avoids concurrent slots
 * latching the same gamescope source and sharing / stalling video).
 */
[[nodiscard]] std::optional<std::string> select_gamescope_pipewire_node_from_dump(
    std::string_view pw_dump_json,
    int expect_width = 0,
    int expect_height = 0,
    int owner_pid = 0);

/**
 * Poll `pw-dump` until a suitable gamescope Video/Source appears or `timeout`
 * elapses. Returns the PipeWire object id as a decimal string for pipewiresrc
 * `path=`.
 */
[[nodiscard]] std::optional<std::string> wait_for_gamescope_pipewire_node(
    std::chrono::milliseconds timeout,
    int expect_width = 0,
    int expect_height = 0,
    int owner_pid = 0);

} // namespace archstreamer
