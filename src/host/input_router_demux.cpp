#include "host/input_router_demux.hpp"

#include <iostream>

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
        } else if (miss_logs_ < 8) {
            ++miss_logs_;
            std::cerr
                << "input demux: no router for client "
                << static_cast<int>(input.client_id)
                << " (registered " << routers_.size() << ")\n";
            for (const auto& [id, _] : routers_) {
                std::cerr << "  registered client " << static_cast<int>(id) << '\n';
            }
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

bool InputRouterDemux::route(const TouchInput& input) {
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
