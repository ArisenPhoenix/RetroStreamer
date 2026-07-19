#include "client/sdl2_controller_backend.hpp"

#include "common/controller_normalization.hpp"
#include "common/time.hpp"

#include <SDL.h>

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <string>
#include <utility>

namespace archstreamer {
namespace {

int parse_device_index(const std::string& id) {
    int value = 0;
    const auto* begin = id.data();
    const auto* end = id.data() + id.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value < 0) {
        throw std::runtime_error("invalid SDL2 controller device id");
    }

    return value;
}

void set_button(
    SDL_GameController* controller,
    SDL_GameControllerButton sdl_button,
    ControllerButton button,
    std::uint32_t& buttons) {
    if (SDL_GameControllerGetButton(controller, sdl_button) != 0) {
        buttons |= button;
    }
}

} // namespace

struct Sdl2ControllerBackend::OpenController {
    LocalPlayerIndex local_player = 0;
    SDL_GameController* controller = nullptr;

    ~OpenController() {
        if (controller != nullptr) {
            SDL_GameControllerClose(controller);
        }
    }
};

Sdl2ControllerBackend::Sdl2ControllerBackend() {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
}

Sdl2ControllerBackend::~Sdl2ControllerBackend() {
    opened_.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

std::vector<ControllerDevice> Sdl2ControllerBackend::list_devices() const {
    const int count = SDL_NumJoysticks();
    if (count < 0) {
        throw std::runtime_error(SDL_GetError());
    }

    std::vector<ControllerDevice> devices;
    for (int i = 0; i < count; ++i) {
        if (SDL_IsGameController(i) != SDL_TRUE) {
            continue;
        }

        const char* name = SDL_GameControllerNameForIndex(i);
        const char* path = SDL_JoystickPathForIndex(i);
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_text[33] = {};
        SDL_JoystickGetGUIDString(guid, guid_text, sizeof(guid_text));

        char* mapping = SDL_GameControllerMappingForDeviceIndex(i);
        devices.push_back(ControllerDevice{
            std::to_string(i),
            name != nullptr ? name : "Unknown controller",
            guid_text,
            path != nullptr ? path : "",
            mapping != nullptr ? mapping : "",
            SDL_JoystickGetDeviceVendor(i),
            SDL_JoystickGetDeviceProduct(i),
        });
        if (mapping != nullptr) {
            SDL_free(mapping);
        }
    }

    return devices;
}

void Sdl2ControllerBackend::open_selected(const std::vector<std::string>& device_ids) {
    if (device_ids.size() > MaxPlayersPerClient) {
        throw std::runtime_error("client can select at most two controllers");
    }

    opened_.clear();
    opened_.reserve(device_ids.size());

    for (std::size_t i = 0; i < device_ids.size(); ++i) {
        SDL_GameController* controller = SDL_GameControllerOpen(parse_device_index(device_ids[i]));
        if (controller == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        auto opened = std::make_unique<OpenController>();
        opened->local_player = static_cast<LocalPlayerIndex>(i);
        opened->controller = controller;
        opened_.push_back(std::move(opened));
    }
}

std::optional<ControllerState> Sdl2ControllerBackend::poll(LocalPlayerIndex local_player) {
    SDL_GameControllerUpdate();

    SDL_GameController* controller = nullptr;
    for (const auto& opened : opened_) {
        if (opened->local_player == local_player) {
            controller = opened->controller;
            break;
        }
    }

    if (controller == nullptr) {
        return std::nullopt;
    }

    ControllerState state;
    state.sequence = next_sequence_++;
    state.timestamp_us = steady_timestamp_us();
    state.left_x = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
    state.left_y = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
    state.right_x = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
    state.right_y = normalize_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
    state.left_trigger = normalize_trigger(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    state.right_trigger = normalize_trigger(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));

    set_button(controller, SDL_CONTROLLER_BUTTON_A, ButtonA, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_B, ButtonB, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_X, ButtonX, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_Y, ButtonY, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_BACK, ButtonBack, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_GUIDE, ButtonGuide, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_START, ButtonStart, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK, ButtonLeftStick, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK, ButtonRightStick, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ButtonLeftShoulder, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ButtonRightShoulder, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_DPAD_UP, ButtonDpadUp, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN, ButtonDpadDown, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT, ButtonDpadLeft, state.buttons);
    set_button(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ButtonDpadRight, state.buttons);

    return state;
}

} // namespace archstreamer
