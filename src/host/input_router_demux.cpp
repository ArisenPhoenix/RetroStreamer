#include "host/input_router_demux.hpp"

namespace archstreamer {

void InputRouterDemux::register_router(ClientId client_id, InputRouter* router) {
    std::lock_guard lock(mutex_);
    if (router == nullptr) {
        routers_.erase(client_id);
        return;
    }
    routers_[client_id] = router;
}

void InputRouterDemux::unregister_router(ClientId client_id) {
    std::lock_guard lock(mutex_);
    routers_.erase(client_id);
}

void InputRouterDemux::unregister_all_for(InputRouter* router) {
    std::lock_guard lock(mutex_);
    for (auto it = routers_.begin(); it != routers_.end();) {
        if (it->second == router) {
            it = routers_.erase(it);
        } else {
            ++it;
        }
    }
}

bool InputRouterDemux::route(const ControllerInput& input) {
    InputRouter* router = nullptr;
    {
        std::lock_guard lock(mutex_);
        const auto it = routers_.find(input.client_id);
        if (it != routers_.end()) {
            router = it->second;
        }
    }
    if (router == nullptr) {
        return false;
    }
    return router->route(input);
}

bool InputRouterDemux::route(const KeyboardInput& input) {
    InputRouter* router = nullptr;
    {
        std::lock_guard lock(mutex_);
        const auto it = routers_.find(input.client_id);
        if (it != routers_.end()) {
            router = it->second;
        }
    }
    if (router == nullptr) {
        return false;
    }
    return router->route(input);
}

} // namespace archstreamer
