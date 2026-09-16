#include "host/contracts/launch.hpp"

#include "host/virtual/pad_plan.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

void upsert(
    std::vector<std::pair<std::string, std::string>>& entries,
    std::string key,
    std::string value) {
    for (auto& entry : entries) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    entries.emplace_back(std::move(key), std::move(value));
}

} // namespace

void ProcessEnvironment::set(std::string key, std::string value) {
    upsert(entries, std::move(key), std::move(value));
}

void ProcessEnvironment::add_unset(std::string key) {
    for (const auto& existing : unset) {
        if (existing == key) {
            return;
        }
    }
    unset.push_back(std::move(key));
}

void ProcessEnvironment::clear_var(const std::string& key) {
    add_unset(key);
    entries.erase(
        std::remove_if(
            entries.begin(),
            entries.end(),
            [&](const auto& entry) { return entry.first == key; }),
        entries.end());
}

void ProcessEnvironment::merge(const ProcessEnvironment& other) {
    for (const auto& key : other.unset) {
        add_unset(key);
    }
    for (const auto& [key, value] : other.entries) {
        upsert(entries, key, value);
    }
}

void ProcessEnvironment::merge_pairs(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
    for (const auto& [key, value] : pairs) {
        upsert(entries, key, value);
    }
}

ProcessEnvironment input_launch_environment(const std::string& ignore_devices) {
    ProcessEnvironment env;
    if (!ignore_devices.empty()) {
        env.set("SDL_GAMECONTROLLER_IGNORE_DEVICES", ignore_devices);
    }
    return env;
}

ProcessEnvironment build_emulator_launch_environment(const EmulatorLaunchEnvRequest& request) {
    ProcessEnvironment env;
    env.merge(audio_launch_environment(
        request.stream_media,
        request.stream_audio,
        request.host_plays_locally,
        request.plug.audio));
    // Pad filters: one applier. Profile env below must not set IGNORE/EXCEPT.
    if (request.plug.seat.has_value()) {
        apply_pad_plan(env, *request.plug.seat);
    } else {
        env.merge(input_launch_environment(request.ignore_devices));
    }
    env.merge(request.gpu_env);
    env.merge(capture_launch_environment(request.plug.present));
    // Pin only: gamescope wrapper reserves lower :N (abstract bind-without-listen
    // + lock files) so nested Xwayland lands here. listen() hang firejail; lock
    // files alone do not reserve abstract names inside a firejail netns.
    // Identity / matching uses ARCHSTREAMER_SESSION_ID → host lease map.
    if (request.plug.present.uses_pipewire() && !request.plug.present.xtest_display.empty()) {
        env.set("ARCHSTREAMER_XTEST_DISPLAY", request.plug.present.xtest_display);
    }
    if (!request.session_id.empty()) {
        env.set("ARCHSTREAMER_SESSION_ID", request.session_id);
    }
    env.merge(request.profile_env);
    // Re-apply pad plan last so profile merge cannot resurrect IGNORE alongside EXCEPT.
    if (request.plug.seat.has_value()) {
        apply_pad_plan(env, *request.plug.seat);
    }
    return env;
}

} // namespace archstreamer
