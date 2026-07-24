#include "client/audio_playback_device.hpp"

#include "client/gstreamer_probe.hpp"
#include "common/platform/process_utils.hpp"

#include <cctype>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

namespace archstreamer {
namespace {

std::string to_lower_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool contains_ci(const std::string& haystack, const char* needle) {
    return to_lower_copy(haystack).find(to_lower_copy(std::string{needle})) != std::string::npos;
}

bool looks_like_controller_audio_device(const std::string& name) {
    return contains_ci(name, "wireless controller") ||
        contains_ci(name, "dualsense") ||
        contains_ci(name, "dualshock") ||
        contains_ci(name, "playstation") ||
        contains_ci(name, "xbox controller") ||
        contains_ci(name, "gamepad") ||
        contains_ci(name, "hands-free");
}

std::string trim_copy(std::string value) {
    while (!value.empty() &&
        (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    return value.substr(start);
}

std::string capture_command_output(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {};
    }
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

#ifdef _WIN32
struct WasapiSinkDevice {
    std::string id;
    std::string name;
    std::string enumerator;
    bool is_default = false;
};

std::vector<WasapiSinkDevice> list_wasapi2_sink_devices() {
    std::vector<WasapiSinkDevice> devices;
    if (!gst_element_available("wasapi2sink")) {
        return devices;
    }
    const auto output = capture_command_output("gst-device-monitor-1.0.exe Audio/Sink");
    WasapiSinkDevice current;
    bool in_device = false;
    auto flush = [&] {
        if (in_device && !current.id.empty()) {
            devices.push_back(current);
        }
        current = {};
        in_device = false;
    };
    std::size_t line_start = 0;
    while (line_start <= output.size()) {
        const auto line_end = output.find('\n', line_start);
        std::string line = output.substr(
            line_start,
            line_end == std::string::npos ? std::string::npos : line_end - line_start);
        line_start = line_end == std::string::npos ? output.size() + 1 : line_end + 1;
        line = trim_copy(std::move(line));
        if (line == "Device found:") {
            flush();
            in_device = true;
            continue;
        }
        if (!in_device) {
            continue;
        }
        constexpr std::string_view name_key = "name  : ";
        constexpr std::string_view id_key = "device.id = ";
        constexpr std::string_view default_key = "device.default = ";
        constexpr std::string_view enumerator_key = "device.enumerator-name = ";
        constexpr std::string_view actual_name_key = "device.actual-name = ";
        if (line.rfind(name_key, 0) == 0) {
            current.name = line.substr(name_key.size());
        } else if (line.rfind(id_key, 0) == 0) {
            current.id = line.substr(id_key.size());
        } else if (line.rfind(default_key, 0) == 0) {
            current.is_default = line.find("true") != std::string::npos;
        } else if (line.rfind(enumerator_key, 0) == 0) {
            current.enumerator = line.substr(enumerator_key.size());
        } else if (line.rfind(actual_name_key, 0) == 0) {
            current.name = line.substr(actual_name_key.size());
        }
    }
    flush();
    return devices;
}

std::optional<WasapiSinkDevice> choose_preferred_wasapi2_device() {
    const auto devices = list_wasapi2_sink_devices();
    auto score = [](const WasapiSinkDevice& device) {
        if (device.id.empty() || looks_like_controller_audio_device(device.name)) {
            return -1000;
        }
        if (contains_ci(device.name, "default audio render device")) {
            return -500;
        }
        int value = 0;
        if (contains_ci(device.enumerator, "HDAUDIO") || contains_ci(device.name, "Realtek")) {
            value += 100;
        }
        if (contains_ci(device.name, "Speakers")) {
            value += 20;
        }
        if (contains_ci(device.enumerator, "USB")) {
            value -= 10;
        }
        if (device.is_default) {
            value += 5;
        }
        return value;
    };

    const WasapiSinkDevice* best = nullptr;
    int best_score = 0;
    for (const auto& device : devices) {
        const int value = score(device);
        if (value > best_score) {
            best_score = value;
            best = &device;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}
#endif

std::string linux_default_pulse_sink() {
#ifndef _WIN32
    return trim_copy(read_command_output("pactl get-default-sink 2>/dev/null"));
#else
    return {};
#endif
}

} // namespace

std::string current_audio_playback_device_key() {
#ifdef _WIN32
    if (const auto device = choose_preferred_wasapi2_device(); device.has_value()) {
        return device->id;
    }
    return "default";
#else
    return linux_default_pulse_sink();
#endif
}

AudioPlaybackSink choose_audio_playback_sink(bool sync) {
    const char* sync_flag = sync ? "true" : "false";
#ifdef _WIN32
    if (const auto device = choose_preferred_wasapi2_device(); device.has_value()) {
        return {
            {
                "wasapi2sink",
                "device=" + device->id,
                std::string("sync=") + sync_flag,
            },
            "wasapi2sink:" + device->name,
            device->id,
        };
    }
    if (gst_element_available("wasapisink")) {
        return {
            {"wasapisink", "role=multimedia", std::string("sync=") + sync_flag},
            "wasapisink role=multimedia",
            "wasapi-default",
        };
    }
    if (gst_element_available("directsoundsink")) {
        return {
            {"directsoundsink", std::string("sync=") + sync_flag},
            "directsoundsink",
            "dsound-default",
        };
    }
#else
    const auto pulse_sink = linux_default_pulse_sink();
    if (!pulse_sink.empty() && gst_element_available("pulsesink")) {
        return {
            {
                "pulsesink",
                "device=" + pulse_sink,
                std::string("sync=") + sync_flag,
            },
            "pulsesink:" + pulse_sink,
            pulse_sink,
        };
    }
#endif
    return {
        {"autoaudiosink", std::string("sync=") + sync_flag},
        sync ? "autoaudiosink sync=true" : "autoaudiosink",
        current_audio_playback_device_key(),
    };
}

} // namespace archstreamer
