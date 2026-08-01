#include "client/audio_playback_device.hpp"

#include "client/gstreamer_probe.hpp"
#include "common/platform/process_utils.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    // Windows GUI builds: _popen() opens a console per gst-device-monitor call.
    const auto output = read_command_output("gst-device-monitor-1.0.exe Audio/Sink");
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
    const auto preferred = preferred_audio_output_device();
    const auto devices = list_wasapi2_sink_devices();
    if (!preferred.empty() && preferred != "auto") {
        for (const auto& device : devices) {
            if (device.id == preferred && !looks_like_controller_audio_device(device.name)) {
                return device;
            }
        }
    }

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

} // namespace

std::vector<AudioOutputDevice> list_audio_output_devices() {
    std::vector<AudioOutputDevice> devices;
    devices.push_back(AudioOutputDevice{"auto", "Auto (system default)", true});
    for (const auto& device : list_wasapi2_sink_devices()) {
        if (device.id.empty() || looks_like_controller_audio_device(device.name)) {
            continue;
        }
        if (contains_ci(device.name, "default audio render device")) {
            continue;
        }
        devices.push_back(AudioOutputDevice{device.id, device.name, device.is_default});
    }
    return devices;
}

std::string current_audio_playback_device_key() {
    if (const auto device = choose_preferred_wasapi2_device(); device.has_value()) {
        return device->id;
    }
    const auto preferred = preferred_audio_output_device();
    return preferred.empty() ? std::string("default") : preferred;
}

AudioPlaybackSink choose_audio_playback_sink(bool sync) {
    const char* sync_flag = sync ? "true" : "false";
    if (const auto device = choose_preferred_wasapi2_device(); device.has_value()) {
        return {
            {
                "wasapi2sink",
                "device=" + device->id,
                // Shared mode + roomier buffers; exclusive/low-latency underruns as choppy audio.
                "exclusive=false",
                "low-latency=false",
                "buffer-time=200000",
                "latency-time=40000",
                std::string("sync=") + sync_flag,
            },
            "wasapi2sink:" + device->name,
            device->id,
        };
    }
    if (gst_element_available("wasapisink")) {
        return {
            {
                "wasapisink",
                "role=multimedia",
                "exclusive=false",
                "low-latency=false",
                "buffer-time=200000",
                "latency-time=40000",
                std::string("sync=") + sync_flag,
            },
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
    return {
        {"autoaudiosink", std::string("sync=") + sync_flag},
        sync ? "autoaudiosink sync=true" : "autoaudiosink",
        current_audio_playback_device_key(),
    };
}

} // namespace archstreamer
