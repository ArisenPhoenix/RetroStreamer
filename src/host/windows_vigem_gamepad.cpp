#include "host/windows_vigem_gamepad.hpp"

#ifdef _WIN32

#include <cstring>
#include <stdexcept>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace archstreamer {
namespace {

// Minimal ViGEmClient C ABI (matches Nefarius.ViGEm.Client).
using VIGEM_ERROR = unsigned int;
constexpr VIGEM_ERROR VIGEM_ERROR_NONE = 0x20000000;

#pragma pack(push, 1)
struct XUSB_REPORT {
    unsigned short wButtons = 0;
    unsigned char bLeftTrigger = 0;
    unsigned char bRightTrigger = 0;
    short sThumbLX = 0;
    short sThumbLY = 0;
    short sThumbRX = 0;
    short sThumbRY = 0;
};
#pragma pack(pop)

constexpr unsigned short XUSB_GAMEPAD_DPAD_UP = 0x0001;
constexpr unsigned short XUSB_GAMEPAD_DPAD_DOWN = 0x0002;
constexpr unsigned short XUSB_GAMEPAD_DPAD_LEFT = 0x0004;
constexpr unsigned short XUSB_GAMEPAD_DPAD_RIGHT = 0x0008;
constexpr unsigned short XUSB_GAMEPAD_START = 0x0010;
constexpr unsigned short XUSB_GAMEPAD_BACK = 0x0020;
constexpr unsigned short XUSB_GAMEPAD_LEFT_THUMB = 0x0040;
constexpr unsigned short XUSB_GAMEPAD_RIGHT_THUMB = 0x0080;
constexpr unsigned short XUSB_GAMEPAD_LEFT_SHOULDER = 0x0100;
constexpr unsigned short XUSB_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr unsigned short XUSB_GAMEPAD_GUIDE = 0x0400;
constexpr unsigned short XUSB_GAMEPAD_A = 0x1000;
constexpr unsigned short XUSB_GAMEPAD_B = 0x2000;
constexpr unsigned short XUSB_GAMEPAD_X = 0x4000;
constexpr unsigned short XUSB_GAMEPAD_Y = 0x8000;

using PFN_vigem_alloc = void* (__cdecl*)();
using PFN_vigem_free = void (__cdecl*)(void*);
using PFN_vigem_connect = VIGEM_ERROR (__cdecl*)(void*);
using PFN_vigem_disconnect = void (__cdecl*)(void*);
using PFN_vigem_target_x360_alloc = void* (__cdecl*)();
using PFN_vigem_target_free = void (__cdecl*)(void*);
using PFN_vigem_target_add = VIGEM_ERROR (__cdecl*)(void*, void*);
using PFN_vigem_target_remove = VIGEM_ERROR (__cdecl*)(void*, void*);
using PFN_vigem_target_x360_update = VIGEM_ERROR (__cdecl*)(void*, void*, XUSB_REPORT);

struct VigemApi {
    HMODULE module = nullptr;
    PFN_vigem_alloc alloc = nullptr;
    PFN_vigem_free free = nullptr;
    PFN_vigem_connect connect = nullptr;
    PFN_vigem_disconnect disconnect = nullptr;
    PFN_vigem_target_x360_alloc target_x360_alloc = nullptr;
    PFN_vigem_target_free target_free = nullptr;
    PFN_vigem_target_add target_add = nullptr;
    PFN_vigem_target_remove target_remove = nullptr;
    PFN_vigem_target_x360_update target_x360_update = nullptr;
};

VigemApi& vigem_api() {
    static VigemApi api;
    return api;
}

bool load_vigem_api() {
    auto& api = vigem_api();
    if (api.module != nullptr) {
        return true;
    }
    api.module = LoadLibraryA("ViGEmClient.dll");
    if (api.module == nullptr) {
        return false;
    }
    api.alloc = reinterpret_cast<PFN_vigem_alloc>(GetProcAddress(api.module, "vigem_alloc"));
    api.free = reinterpret_cast<PFN_vigem_free>(GetProcAddress(api.module, "vigem_free"));
    api.connect = reinterpret_cast<PFN_vigem_connect>(GetProcAddress(api.module, "vigem_connect"));
    api.disconnect =
        reinterpret_cast<PFN_vigem_disconnect>(GetProcAddress(api.module, "vigem_disconnect"));
    api.target_x360_alloc = reinterpret_cast<PFN_vigem_target_x360_alloc>(
        GetProcAddress(api.module, "vigem_target_x360_alloc"));
    api.target_free =
        reinterpret_cast<PFN_vigem_target_free>(GetProcAddress(api.module, "vigem_target_free"));
    api.target_add =
        reinterpret_cast<PFN_vigem_target_add>(GetProcAddress(api.module, "vigem_target_add"));
    api.target_remove =
        reinterpret_cast<PFN_vigem_target_remove>(GetProcAddress(api.module, "vigem_target_remove"));
    api.target_x360_update = reinterpret_cast<PFN_vigem_target_x360_update>(
        GetProcAddress(api.module, "vigem_target_x360_update"));
    if (api.alloc == nullptr || api.free == nullptr || api.connect == nullptr ||
        api.disconnect == nullptr || api.target_x360_alloc == nullptr ||
        api.target_free == nullptr || api.target_add == nullptr ||
        api.target_remove == nullptr || api.target_x360_update == nullptr) {
        FreeLibrary(api.module);
        api = {};
        return false;
    }
    return true;
}

XUSB_REPORT to_xusb_report(const ControllerState& state) {
    XUSB_REPORT report{};
    auto set = [&](ControllerButton button, unsigned short mask) {
        if ((state.buttons & button) != 0) {
            report.wButtons = static_cast<unsigned short>(report.wButtons | mask);
        }
    };
    set(ButtonA, XUSB_GAMEPAD_A);
    set(ButtonB, XUSB_GAMEPAD_B);
    set(ButtonX, XUSB_GAMEPAD_X);
    set(ButtonY, XUSB_GAMEPAD_Y);
    set(ButtonBack, XUSB_GAMEPAD_BACK);
    set(ButtonGuide, XUSB_GAMEPAD_GUIDE);
    set(ButtonStart, XUSB_GAMEPAD_START);
    set(ButtonLeftStick, XUSB_GAMEPAD_LEFT_THUMB);
    set(ButtonRightStick, XUSB_GAMEPAD_RIGHT_THUMB);
    set(ButtonLeftShoulder, XUSB_GAMEPAD_LEFT_SHOULDER);
    set(ButtonRightShoulder, XUSB_GAMEPAD_RIGHT_SHOULDER);
    set(ButtonDpadUp, XUSB_GAMEPAD_DPAD_UP);
    set(ButtonDpadDown, XUSB_GAMEPAD_DPAD_DOWN);
    set(ButtonDpadLeft, XUSB_GAMEPAD_DPAD_LEFT);
    set(ButtonDpadRight, XUSB_GAMEPAD_DPAD_RIGHT);
    report.bLeftTrigger = static_cast<unsigned char>(state.left_trigger >> 8);
    report.bRightTrigger = static_cast<unsigned char>(state.right_trigger >> 8);
    report.sThumbLX = state.left_x;
    report.sThumbLY = static_cast<short>(-state.left_y); // XInput Y is inverted vs SDL
    report.sThumbRX = state.right_x;
    report.sThumbRY = static_cast<short>(-state.right_y);
    return report;
}

} // namespace

ViGEmGamepadBus::ViGEmGamepadBus(VirtualGamepadIdentity identity)
    : identity_(std::move(identity)) {
    if (identity_.name.empty()) {
        identity_.name = "ArchStreamer Virtual Gamepad";
    }
}

ViGEmGamepadBus::ViGEmGamepadBus(std::vector<VirtualGamepadIdentity> identities)
    : identities_(std::move(identities)) {
    if (identities_.empty()) {
        identity_.name = "ArchStreamer Virtual Gamepad";
    }
}

ViGEmGamepadBus::~ViGEmGamepadBus() {
    for (RetroArchPort port = 0; port < pads_.size(); ++port) {
        unplug(port);
    }
    if (client_ != nullptr) {
        auto& api = vigem_api();
        if (api.disconnect != nullptr) {
            api.disconnect(client_);
        }
        if (api.free != nullptr) {
            api.free(client_);
        }
        client_ = nullptr;
    }
}

void ViGEmGamepadBus::ensure_client() {
    if (client_ != nullptr) {
        return;
    }
    if (!load_vigem_api()) {
        throw std::runtime_error(
            "ViGEmClient.dll not found. Install ViGEmBus and place ViGEmClient.dll on PATH "
            "(see deploy/windows/install-deps.ps1).");
    }
    auto& api = vigem_api();
    client_ = api.alloc();
    if (client_ == nullptr) {
        throw std::runtime_error("vigem_alloc failed");
    }
    const auto err = api.connect(client_);
    if (err != VIGEM_ERROR_NONE) {
        api.free(client_);
        client_ = nullptr;
        throw std::runtime_error(
            "vigem_connect failed (is the ViGEmBus driver installed?). Error=0x" +
            std::to_string(err));
    }
}

ViGEmGamepadBus::Pad& ViGEmGamepadBus::pad_for(RetroArchPort port) {
    if (port >= pads_.size()) {
        throw std::runtime_error("virtual pad port out of range");
    }
    return pads_[port];
}

void ViGEmGamepadBus::plug(RetroArchPort port) {
    auto& pad = pad_for(port);
    if (pad.plugged) {
        return;
    }
    ensure_client();
    auto& api = vigem_api();
    pad.target = api.target_x360_alloc();
    if (pad.target == nullptr) {
        throw std::runtime_error("vigem_target_x360_alloc failed");
    }
    const auto err = api.target_add(client_, pad.target);
    if (err != VIGEM_ERROR_NONE) {
        api.target_free(pad.target);
        pad.target = nullptr;
        throw std::runtime_error("vigem_target_add failed (0x" + std::to_string(err) + ")");
    }
    pad.plugged = true;
    (void)identity_;
    (void)identities_;
}

void ViGEmGamepadBus::unplug(RetroArchPort port) {
    auto& pad = pad_for(port);
    if (!pad.plugged) {
        return;
    }
    auto& api = vigem_api();
    if (client_ != nullptr && pad.target != nullptr && api.target_remove != nullptr) {
        api.target_remove(client_, pad.target);
    }
    if (pad.target != nullptr && api.target_free != nullptr) {
        api.target_free(pad.target);
    }
    pad.target = nullptr;
    pad.plugged = false;
    pad.has_last = false;
    pad.last = {};
}

void ViGEmGamepadBus::update(RetroArchPort port, const ControllerState& state) {
    auto& pad = pad_for(port);
    if (!pad.plugged) {
        plug(port);
    }
    if (pad.has_last &&
        pad.last.buttons == state.buttons &&
        pad.last.left_x == state.left_x &&
        pad.last.left_y == state.left_y &&
        pad.last.right_x == state.right_x &&
        pad.last.right_y == state.right_y &&
        pad.last.left_trigger == state.left_trigger &&
        pad.last.right_trigger == state.right_trigger) {
        return;
    }
    auto& api = vigem_api();
    const auto report = to_xusb_report(state);
    const auto err = api.target_x360_update(client_, pad.target, report);
    if (err != VIGEM_ERROR_NONE) {
        throw std::runtime_error("vigem_target_x360_update failed (0x" + std::to_string(err) + ")");
    }
    pad.last = state;
    pad.has_last = true;
}

} // namespace archstreamer

#endif // _WIN32
