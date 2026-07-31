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

constexpr std::uint16_t ArchStreamerVirtualVendorId = 0x1209;
constexpr std::uint16_t ArchStreamerVirtualProductIdBase = 0xa517;
constexpr auto kReopenBackoff = std::chrono::milliseconds(400);

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

bool is_archstreamer_virtual_device(const ControllerDevice& device) {
    if (device.name.rfind("ArchStreamer", 0) == 0) {
        return true;
    }

    return device.vendor_id == ArchStreamerVirtualVendorId &&
        device.product_id >= ArchStreamerVirtualProductIdBase &&
        device.product_id < ArchStreamerVirtualProductIdBase + MaxRetroArchPorts;
}

ControllerDevice device_at_index(int index) {
    const char* name = SDL_GameControllerNameForIndex(index);
    const char* path = SDL_JoystickPathForIndex(index);
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
    char guid_text[33] = {};
    SDL_JoystickGetGUIDString(guid, guid_text, sizeof(guid_text));

    char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
    auto device = ControllerDevice{
        std::to_string(index),
        name != nullptr ? name : "Unknown controller",
        guid_text,
        path != nullptr ? path : "",
        mapping != nullptr ? mapping : "",
        SDL_JoystickGetDeviceVendor(index),
        SDL_JoystickGetDeviceProduct(index),
    };
    if (mapping != nullptr) {
        SDL_free(mapping);
    }
    return device;
}

bool index_in_use(const std::vector<std::int32_t>& used, int device_index) {
    const auto instance = static_cast<std::int32_t>(SDL_JoystickGetDeviceInstanceID(device_index));
    if (instance < 0) {
        return true;
    }
    for (const std::int32_t value : used) {
        if (value == instance) {
            return true;
        }
    }
    return false;
}

ControllerState make_neutral_state(std::uint32_t sequence) {
    ControllerState state;
    state.sequence = sequence;
    state.timestamp_us = steady_timestamp_us();
    return state;
}

} // namespace

struct Sdl2ControllerBackend::OpenController {
    LocalPlayerIndex local_player = 0;
    SDL_GameController* controller = nullptr;
    SDL_JoystickID instance_id = -1;
    std::string preferred_guid;
    std::string preferred_path;
    std::string preferred_name;
    std::uint16_t preferred_vendor = 0;
    std::uint16_t preferred_product = 0;

    ~OpenController() {
        if (controller != nullptr) {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
};

Sdl2ControllerBackend::Sdl2ControllerBackend() {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        throw std::runtime_error(SDL_GetError());
    }
    // Required for SDL_NumJoysticks / device list to update on plug/unplug.
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);
}

Sdl2ControllerBackend::~Sdl2ControllerBackend() {
    opened_.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

std::vector<ControllerDevice> Sdl2ControllerBackend::list_devices() const {
    SDL_PumpEvents();
    const int count = SDL_NumJoysticks();
    if (count < 0) {
        throw std::runtime_error(SDL_GetError());
    }

    std::vector<ControllerDevice> devices;
    for (int i = 0; i < count; ++i) {
        if (SDL_IsGameController(i) != SDL_TRUE) {
            continue;
        }

        auto device = device_at_index(i);
        if (is_archstreamer_virtual_device(device)) {
            continue;
        }

        devices.push_back(std::move(device));
    }

    return devices;
}

void Sdl2ControllerBackend::open_selected(const std::vector<std::string>& device_ids) {
    if (device_ids.size() > MaxPlayersPerClient) {
        throw std::runtime_error("client can select at most two controllers");
    }

    opened_.clear();
    opened_.reserve(device_ids.size());
    SDL_PumpEvents();

    for (std::size_t i = 0; i < device_ids.size(); ++i) {
        const int index = parse_device_index(device_ids[i]);
        if (SDL_IsGameController(index) != SDL_TRUE) {
            throw std::runtime_error("selected device is not an SDL game controller");
        }

        const auto info = device_at_index(index);
        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (controller == nullptr) {
            throw std::runtime_error(SDL_GetError());
        }

        auto opened = std::make_unique<OpenController>();
        opened->local_player = static_cast<LocalPlayerIndex>(i);
        opened->controller = controller;
        if (SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
            joystick != nullptr) {
            opened->instance_id = SDL_JoystickInstanceID(joystick);
        }
        opened->preferred_guid = info.guid;
        opened->preferred_path = info.path;
        opened->preferred_name = info.name;
        opened->preferred_vendor = info.vendor_id;
        opened->preferred_product = info.product_id;
        opened_.push_back(std::move(opened));
    }

    next_reopen_attempt_ = std::chrono::steady_clock::now();
}

std::vector<std::int32_t> Sdl2ControllerBackend::used_instance_ids() const {
    std::vector<std::int32_t> used;
    used.reserve(opened_.size());
    for (const auto& opened : opened_) {
        if (opened && opened->controller != nullptr && opened->instance_id >= 0) {
            used.push_back(static_cast<std::int32_t>(opened->instance_id));
        }
    }
    return used;
}

void Sdl2ControllerBackend::push_hotplug_status(std::string message) {
    std::lock_guard lock(status_mutex_);
    pending_status_ = std::move(message);
}

std::optional<std::string> Sdl2ControllerBackend::take_hotplug_status() {
    std::lock_guard lock(status_mutex_);
    auto out = pending_status_;
    pending_status_.reset();
    return out;
}

void Sdl2ControllerBackend::close_slot(OpenController& slot, const char* reason) {
    const auto name = slot.preferred_name.empty() ? std::string("controller") : slot.preferred_name;
    if (slot.controller != nullptr) {
        SDL_GameControllerClose(slot.controller);
        slot.controller = nullptr;
    }
    slot.instance_id = -1;
    push_hotplug_status(
        "Controller P" + std::to_string(static_cast<int>(slot.local_player) + 1) +
        " (" + name + ") " + reason +
        ". Plug it back in (or another pad) — no host reconnect needed.");
}

bool Sdl2ControllerBackend::try_open_slot(OpenController& slot) {
    const int count = SDL_NumJoysticks();
    if (count < 0) {
        return false;
    }

    const auto used = used_instance_ids();
    auto try_index = [&](int index, bool require_preferred_match) -> bool {
        if (index < 0 || index >= count || index_in_use(used, index)) {
            return false;
        }
        if (SDL_IsGameController(index) != SDL_TRUE) {
            return false;
        }
        const auto info = device_at_index(index);
        if (is_archstreamer_virtual_device(info)) {
            return false;
        }
        if (require_preferred_match) {
            const bool guid_match =
                !slot.preferred_guid.empty() && info.guid == slot.preferred_guid;
            const bool path_match =
                !slot.preferred_path.empty() && info.path == slot.preferred_path;
            if (!guid_match && !path_match) {
                return false;
            }
        }

        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (controller == nullptr) {
            return false;
        }

        slot.controller = controller;
        slot.instance_id = -1;
        if (SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
            joystick != nullptr) {
            slot.instance_id = SDL_JoystickInstanceID(joystick);
        }
        // Remember whatever we claimed so the next unplug prefers this pad.
        slot.preferred_guid = info.guid;
        slot.preferred_path = info.path;
        slot.preferred_name = info.name;
        slot.preferred_vendor = info.vendor_id;
        slot.preferred_product = info.product_id;
        push_hotplug_status(
            "Controller P" + std::to_string(static_cast<int>(slot.local_player) + 1) +
            " reconnected: " + info.name);
        return true;
    };

    // Prefer the originally selected pad (same GUID/path), then any free pad.
    for (int index = 0; index < count; ++index) {
        if (try_index(index, true)) {
            return true;
        }
    }
    for (int index = 0; index < count; ++index) {
        if (try_index(index, false)) {
            return true;
        }
    }
    return false;
}

void Sdl2ControllerBackend::refresh_hotplug() {
    // Drain the event queue so joystick hotplug updates SDL_NumJoysticks().
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        // Intentionally ignore payload — rescanning indexes is enough.
    }

    bool need_reopen = false;
    for (auto& opened : opened_) {
        if (!opened) {
            continue;
        }
        if (opened->controller == nullptr) {
            need_reopen = true;
            continue;
        }
        if (SDL_GameControllerGetAttached(opened->controller) != SDL_TRUE) {
            close_slot(*opened, "disconnected");
            need_reopen = true;
        }
    }

    if (!need_reopen) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_reopen_attempt_) {
        return;
    }
    next_reopen_attempt_ = now + kReopenBackoff;

    for (auto& opened : opened_) {
        if (!opened || opened->controller != nullptr) {
            continue;
        }
        try_open_slot(*opened);
    }
}

std::optional<ControllerState> Sdl2ControllerBackend::poll(LocalPlayerIndex local_player) {
    refresh_hotplug();
    SDL_GameControllerUpdate();

    OpenController* slot = nullptr;
    for (auto& opened : opened_) {
        if (opened && opened->local_player == local_player) {
            slot = opened.get();
            break;
        }
    }

    if (slot == nullptr) {
        return std::nullopt;
    }

    // Keep sending neutrals while unplugged so the host uinput pad does not stick.
    if (slot->controller == nullptr ||
        SDL_GameControllerGetAttached(slot->controller) != SDL_TRUE) {
        return make_neutral_state(next_sequence_++);
    }

    SDL_GameController* controller = slot->controller;
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

    // Some paths (virtio-evdev, odd HID adapters) expose the D-pad only as hat 0 while
    // SDL_GameControllerGetButton(DPAD_*) stays zero. Merge hat bits into the *same*
    // ButtonDpad* flags. On a normal DualShock/Xbox pad the GameController mapping
    // already covers the D-pad, so this is a no-op or a redundant OR — never a second
    // logical press for RetroArch (the host emits each bit once).
    if (SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
        joystick != nullptr && SDL_JoystickNumHats(joystick) > 0) {
        const Uint8 hat = SDL_JoystickGetHat(joystick, 0);
        if ((hat & SDL_HAT_UP) != 0) {
            state.buttons |= ButtonDpadUp;
        }
        if ((hat & SDL_HAT_DOWN) != 0) {
            state.buttons |= ButtonDpadDown;
        }
        if ((hat & SDL_HAT_LEFT) != 0) {
            state.buttons |= ButtonDpadLeft;
        }
        if ((hat & SDL_HAT_RIGHT) != 0) {
            state.buttons |= ButtonDpadRight;
        }
    }

    return state;
}

} // namespace archstreamer
