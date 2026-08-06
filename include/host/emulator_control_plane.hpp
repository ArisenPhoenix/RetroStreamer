#pragma once

#include "common/protocol.hpp"

namespace archstreamer {

class VirtualKeyboard;

/**
 * ArchStreamer-standard emulator control intents.
 * Clients speak these meanings; backends (RetroArch / Ryujinx / melonDS) apply them.
 *
 * Stateful kinds: 1..63 — desired condition (absolute On/Off, later rates).
 * Action kinds: 64+ — one-shot edge (save, disc, …) when added.
 */
enum class EmulatorIntentKind : std::uint8_t {
    Pause = 1,
    FastForward = 2,
    // FastForwardRate = 3, // future stateful
    ScreenSwap = 64,
};

enum class EmulatorIntentClass : std::uint8_t {
    Stateful = 0,
    Action = 1,
};

inline EmulatorIntentClass emulator_intent_class(EmulatorIntentKind kind) {
    return static_cast<std::uint8_t>(kind) >= 64
        ? EmulatorIntentClass::Action
        : EmulatorIntentClass::Stateful;
}

/** Which actuator family VirtualKeyboard / netcmd should use. */
enum class EmulatorControlBackend : std::uint8_t {
    RetroArch = 0,
    Ryujinx = 1,
    MelonDS = 2,
};

/**
 * Single host ingress for client EmulatorControl packets (and, later, remoted
 * key redirects into the same semantic path).
 *
 * Expands pause/FF fields into intents, then dispatches by class to the
 * backend-aware VirtualKeyboard actuators.
 */
class EmulatorControlPlane {
public:
    explicit EmulatorControlPlane(VirtualKeyboard* keyboard = nullptr);

    void set_keyboard(VirtualKeyboard* keyboard);
    void set_backend(EmulatorControlBackend backend);
    EmulatorControlBackend backend() const { return backend_; }

    /** Client → host EmulatorControl packet. */
    void apply_from_client(const EmulatorControl& control);

private:
    void apply_stateful(EmulatorIntentKind kind, bool want_on, bool force);
    void apply_action(EmulatorIntentKind kind);

    VirtualKeyboard* keyboard_ = nullptr;
    EmulatorControlBackend backend_ = EmulatorControlBackend::RetroArch;
};

} // namespace archstreamer
