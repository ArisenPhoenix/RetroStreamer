#pragma once

#include "common/protocol.hpp"
#include "host/input_router.hpp"

#include <mutex>
#include <unordered_map>

namespace archstreamer {

/** Thread-safe client_id → InputRouter demux for concurrent session slots. */
class InputRouterDemux {
public:
    void register_router(ClientId client_id, InputRouter* router);
    void unregister_router(ClientId client_id);
    void unregister_all_for(InputRouter* router);

    bool route(const ControllerInput& input);
    bool route(const KeyboardInput& input);
    bool route(const TouchInput& input);

private:
    mutable std::mutex mutex_;
    std::unordered_map<ClientId, InputRouter*> routers_;
    int miss_logs_ = 0;
};

} // namespace archstreamer
