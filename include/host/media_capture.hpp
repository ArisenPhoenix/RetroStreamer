#pragma once

#include "common/protocol.hpp"

namespace archstreamer {

enum class VirtualDisplayBackend {
    None,
    Xvfb,
    Xephyr,
};

enum class AudioCaptureBackend {
    Pulse,
    PipeWire,
};

} // namespace archstreamer
