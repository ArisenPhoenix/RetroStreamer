#include "client/controller_backend.hpp"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

namespace {

void print_state(const ControllerState& state, bool live_mode) {
    std::cout
        << (live_mode ? "\r" : "")
        << "seq=" << std::setw(4) << state.sequence
        << " ts_us=" << state.timestamp_us
        << " buttons=0x" << std::hex << state.buttons << std::dec
        << " lx=" << std::setw(6) << state.left_x
        << " ly=" << std::setw(6) << state.left_y
        << " rx=" << std::setw(6) << state.right_x
        << " ry=" << std::setw(6) << state.right_y
        << " lt=" << std::setw(5) << state.left_trigger
        << " rt=" << std::setw(5) << state.right_trigger
        << "        ";

    if (live_mode) {
        std::cout << std::flush;
    } else {
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace archstreamer;

    try {
        ControllerBackend backend;
        const auto devices = backend.list_devices();

        if (devices.empty()) {
            std::cout << "No SDL2 game controllers detected.\n";
            return 1;
        }

        std::cout << "Detected gamepads:\n";
        for (std::size_t i = 0; i < devices.size(); ++i) {
            std::cout << "  " << i << ": " << devices[i].name << " [id=" << devices[i].id << "]\n";
            std::cout << "     guid=" << devices[i].guid << '\n';
            std::cout
                << "     vid/pid=0x" << std::hex << std::setw(4) << std::setfill('0') << devices[i].vendor_id
                << "/0x" << std::setw(4) << devices[i].product_id
                << std::dec << std::setfill(' ') << '\n';
            if (!devices[i].path.empty()) {
                std::cout << "     path=" << devices[i].path << '\n';
            }
            if (!devices[i].mapping.empty()) {
                std::cout << "     mapping=" << devices[i].mapping << '\n';
            }
        }

        std::size_t selected_index = 0;
        std::size_t max_samples = 0;
        if (argc > 1) {
            selected_index = static_cast<std::size_t>(std::stoul(argv[1]));
        }
        if (argc > 2) {
            max_samples = static_cast<std::size_t>(std::stoul(argv[2]));
        }

        if (selected_index >= devices.size()) {
            std::cerr << "Selected index is out of range.\n";
            return 1;
        }

        backend.open_selected({devices[selected_index].id});
        std::cout << "Polling " << devices[selected_index].name << ". Press Ctrl+C to stop.\n";

        std::size_t samples = 0;
        const bool live_mode = max_samples == 0;
        while (max_samples == 0 || samples < max_samples) {
            const auto state = backend.poll(0);
            if (state.has_value()) {
                print_state(*state, live_mode);
                ++samples;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

        if (live_mode) {
            std::cout << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "controller_probe: " << error.what() << '\n';
        return 1;
    }
}
