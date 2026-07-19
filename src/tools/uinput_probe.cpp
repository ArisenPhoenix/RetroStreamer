#include "host/linux_uinput_gamepad.hpp"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using namespace archstreamer;

    try {
        LinuxUinputGamepadBus bus;
        bus.plug(0);

        ControllerState state;
        state.timestamp_us = 1;
        state.buttons = ButtonA;
        bus.update(0, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        state.timestamp_us = 2;
        state.buttons = ButtonDpadUp;
        bus.update(0, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        state.timestamp_us = 3;
        state.buttons = ButtonDpadRight;
        bus.update(0, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        state.timestamp_us = 4;
        state.buttons = ButtonDpadDown;
        bus.update(0, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        state.timestamp_us = 5;
        state.buttons = ButtonDpadLeft;
        bus.update(0, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        state.buttons = 0;
        bus.update(0, state);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        bus.unplug(0);
        std::cout << "Created and updated one virtual ArchStreamer gamepad.\n";
    } catch (const std::exception& error) {
        std::cerr << "uinput_probe: " << error.what() << '\n';
        return 1;
    }
}
