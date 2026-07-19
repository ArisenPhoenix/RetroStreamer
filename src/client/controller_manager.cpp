#include "client/controller_manager.hpp"

#include <stdexcept>
#include <utility>

namespace archstreamer {

void ControllerManager::set_available_devices(std::vector<ControllerDevice> devices) {
    devices_ = std::move(devices);
    if (selected_.size() > devices_.size()) {
        selected_.resize(devices_.size());
    }
}

const std::vector<ControllerDevice>& ControllerManager::available_devices() const {
    return devices_;
}

const std::vector<SelectedController>& ControllerManager::selected_controllers() const {
    return selected_;
}

void ControllerManager::select_viewer_only() {
    selected_.clear();
}

void ControllerManager::select_controllers(std::vector<SelectedController> selected) {
    if (selected.size() > MaxPlayersPerClient) {
        throw std::runtime_error("client can select at most two controllers");
    }

    for (std::size_t i = 0; i < selected.size(); ++i) {
        selected[i].local_player = static_cast<LocalPlayerIndex>(i);
    }

    selected_ = std::move(selected);
}

std::uint8_t ControllerManager::requested_player_count() const {
    return static_cast<std::uint8_t>(selected_.size());
}

} // namespace archstreamer
