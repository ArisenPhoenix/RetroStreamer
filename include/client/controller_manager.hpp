#pragma once

#include "common/controller_state.hpp"
#include "common/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace archstreamer {

struct ControllerDevice {
    std::string id;
    std::string name;
    std::string guid;
    std::string path;
    std::string mapping;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
};

struct SelectedController {
    ControllerDevice device;
    LocalPlayerIndex local_player = 0;
};

class ControllerManager {
public:
    void set_available_devices(std::vector<ControllerDevice> devices);
    const std::vector<ControllerDevice>& available_devices() const;
    const std::vector<SelectedController>& selected_controllers() const;
    void select_viewer_only();
    void select_controllers(std::vector<SelectedController> selected);
    std::uint8_t requested_player_count() const;

private:
    std::vector<ControllerDevice> devices_;
    std::vector<SelectedController> selected_;
};

} // namespace archstreamer
