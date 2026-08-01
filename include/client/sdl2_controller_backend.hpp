#pragma once

#include "client/controller_manager.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct _SDL_GameController;
using SDL_GameController = struct _SDL_GameController;

namespace archstreamer {

class Sdl2ControllerBackend final {
public:
    Sdl2ControllerBackend();
    ~Sdl2ControllerBackend();

    Sdl2ControllerBackend(const Sdl2ControllerBackend&) = delete;
    Sdl2ControllerBackend& operator=(const Sdl2ControllerBackend&) = delete;

    std::vector<ControllerDevice> list_devices() const;
    void open_selected(const std::vector<std::string>& device_ids);
    // Always returns a state for an opened local-player slot (live or neutral).
    // Mid-session: drops dead pads, reopens the exact same device (path) when
    // possible; otherwise claims the next free pad that produces input (never
    // steals a pad already bound to another local player).
    std::optional<ControllerState> poll(LocalPlayerIndex local_player);
    // Non-empty when a pad was lost or (re)claimed since the last take.
    std::optional<std::string> take_hotplug_status();

private:
    struct OpenController;

    void refresh_hotplug();
    void close_slot(OpenController& slot, const char* reason);
    bool try_open_slot(OpenController& slot);
    void push_hotplug_status(std::string message);
    std::vector<std::int32_t> used_instance_ids() const;

    std::vector<std::unique_ptr<OpenController>> opened_;
    std::uint32_t next_sequence_ = 1;
    std::chrono::steady_clock::time_point next_reopen_attempt_{};
    mutable std::mutex status_mutex_;
    std::optional<std::string> pending_status_;
};

} // namespace archstreamer
