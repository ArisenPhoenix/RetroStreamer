#pragma once

#include "../common/controller_state.hpp"
#include "../common/protocol.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

struct ControllerDevice {
    std::string id;
    std::string name;
};

struct SelectedController {
    ControllerDevice device;
    LocalPlayerIndex local_player = 0;
};

class ControllerManager {
public:
    void set_available_devices(std::vector<ControllerDevice> devices) {
        devices_ = std::move(devices);
        if (selected_.size() > devices_.size()) {
            selected_.resize(devices_.size());
        }
    }

    const std::vector<ControllerDevice>& available_devices() const {
        return devices_;
    }

    const std::vector<SelectedController>& selected_controllers() const {
        return selected_;
    }

    void select_viewer_only() {
        selected_.clear();
    }

    void select_controllers(std::vector<SelectedController> selected) {
        if (selected.size() > MaxPlayersPerClient) {
            throw std::runtime_error("client can select at most two controllers");
        }

        for (std::size_t i = 0; i < selected.size(); ++i) {
            selected[i].local_player = static_cast<LocalPlayerIndex>(i);
        }

        selected_ = std::move(selected);
    }

    std::uint8_t requested_player_count() const {
        return static_cast<std::uint8_t>(selected_.size());
    }

private:
    std::vector<ControllerDevice> devices_;
    std::vector<SelectedController> selected_;
};

} // namespace archstreamer
