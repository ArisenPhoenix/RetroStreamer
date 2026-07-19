#pragma once

#include "client/controller_manager.hpp"

#include <memory>
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
    std::optional<ControllerState> poll(LocalPlayerIndex local_player);

private:
    struct OpenController;

    std::vector<std::unique_ptr<OpenController>> opened_;
    std::uint32_t next_sequence_ = 1;
};

} // namespace archstreamer
