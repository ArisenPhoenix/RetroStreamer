#pragma once

#include "host/contracts/present.hpp"

#include <cstdint>
#include <string>

namespace archstreamer {

/**
 * Host-facing video knobs. Video owns this; HostAppConfig embeds it so session
 * does not keep a parallel flag bag (ports, display, resolution, backend).
 */
struct VideoSettings {
    bool enabled = true;
    std::string destination = "127.0.0.1";
    bool destination_explicit = false;
    std::uint16_t port = 5004;
    std::string virtual_display = ":99";
    std::string resolution = "1920x1080";
    /** Hint for resolve_present_target; None lets the platform pick. */
    VirtualDisplayBackend preferred_backend = VirtualDisplayBackend::None;
};

} // namespace archstreamer
