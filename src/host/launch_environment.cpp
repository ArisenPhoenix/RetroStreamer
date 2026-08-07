#include "host/launch_environment.hpp"

#include "host/nds/melonds_user_profile.hpp"

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
        request.audio_source));
    // Pad filters: one applier. Profile env below must not set IGNORE/EXCEPT.
    if (request.pad_plan.has_value()) {
        apply_pad_plan(env, *request.pad_plan);
    } else {
        env.merge(input_launch_environment(request.ignore_devices));
    }
    if (request.render_gpu.has_value()) {
        env.merge_pairs(render_gpu_environment(*request.render_gpu));
    }
    env.merge(capture_launch_environment(
        request.use_virtual_capture,
        request.gamescope_capture,
        request.virtualgl_capture,
        request.capture_display));
    // Pin only: gamescope wrapper reserves lower :N (abstract bind-without-listen
    // + lock files) so nested Xwayland lands here. listen() hang firejail; lock
    // files alone do not reserve abstract names inside a firejail netns.
    // Identity / matching uses ARCHSTREAMER_SESSION_ID → host lease map.
    if (request.gamescope_capture && !request.xtest_display.empty()) {
        env.set("ARCHSTREAMER_XTEST_DISPLAY", request.xtest_display);
    }
    if (!request.session_id.empty()) {
        env.set("ARCHSTREAMER_SESSION_ID", request.session_id);
    }
    if (request.yuzu_profile.has_value()) {
        env.merge_pairs(yuzu_launch_environment(*request.yuzu_profile));
    }
    if (request.ryujinx_profile.has_value()) {
        env.merge_pairs(ryujinx_launch_environment(*request.ryujinx_profile));
    }
    if (request.melonds_profile.has_value()) {
        env.merge_pairs(melonds_launch_environment(*request.melonds_profile));
    }
    // Re-apply pad plan last so profile merge cannot resurrect IGNORE alongside EXCEPT.
    if (request.pad_plan.has_value()) {
        apply_pad_plan(env, *request.pad_plan);
    }
    return env;
}

} // namespace archstreamer
