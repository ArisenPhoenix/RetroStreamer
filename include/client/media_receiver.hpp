#pragma once

#include "common/media.hpp"

namespace archstreamer {

class MediaReceiver {
public:
    virtual ~MediaReceiver() = default;

    virtual void connect(const MediaEndpoint& endpoint) = 0;
    virtual void disconnect() = 0;
};

} // namespace archstreamer
