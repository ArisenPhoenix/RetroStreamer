#include "client/audio_playback_device.hpp"

#include "client/gstreamer_probe.hpp"
#include "common/platform/process_utils.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace archstreamer {
namespace {

std::mutex g_preferred_mutex;
std::string g_preferred_audio_output = "auto";
std::atomic<std::uint64_t> g_preferred_epoch{1};

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
    // Windows GUI builds: _popen() opens a console per gst-device-monitor call.
    return read_command_output(command.c_str());
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
#endif

bool is_archstreamer_capture_sink(const std::string& name) {
    return name == "archstreamer" || name.rfind("archstreamer", 0) == 0;
}

std::string linux_default_pulse_sink() {
#ifndef _WIN32
    return trim_copy(read_command_output("pactl get-default-sink 2>/dev/null"));
#else
    return {};
#endif
}

std::string sink_description_from_pactl_list(const std::string& dump, const std::string& sink_name) {
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto next = dump.find("Sink #", pos + 1);
        const auto block = dump.substr(
            pos,
            next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? dump.size() : next;

        const auto name_key = block.find("Name: ");
        if (name_key == std::string::npos) {
            continue;
        }
        auto name = block.substr(name_key + 6);
        const auto name_nl = name.find('\n');
        if (name_nl != std::string::npos) {
            name = name.substr(0, name_nl);
        }
        if (trim_copy(name) != sink_name) {
            continue;
        }
        const auto desc_key = block.find("Description: ");
        if (desc_key == std::string::npos) {
            return sink_name;
        }
        auto desc = block.substr(desc_key + 13);
        const auto desc_nl = desc.find('\n');
        if (desc_nl != std::string::npos) {
            desc = desc.substr(0, desc_nl);
        }
        desc = trim_copy(std::move(desc));
        return desc.empty() ? sink_name : desc;
    }
    return sink_name;
}

std::vector<AudioOutputDevice> list_pulse_output_devices() {
    std::vector<AudioOutputDevice> devices;
#ifndef _WIN32
    const auto defaults = linux_default_pulse_sink();
    const auto details = read_command_output("pactl list sinks 2>/dev/null");
    const auto short_list = read_command_output("pactl list short sinks 2>/dev/null");
    std::string::size_type pos = 0;
    while (pos < short_list.size()) {
        const auto end = short_list.find('\n', pos);
        const auto line = short_list.substr(
            pos,
            end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? short_list.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        const auto tab1 = line.find('\t');
        if (tab1 == std::string::npos) {
            continue;
        }
        auto rest = line.substr(tab1 + 1);
        const auto tab2 = rest.find('\t');
        const auto name = tab2 == std::string::npos ? rest : rest.substr(0, tab2);
        if (name.empty()) {
            continue;
        }
        // Hide ArchStreamer capture null sinks (and any leftover park sinks).
        if (is_archstreamer_capture_sink(name)) {
            continue;
        }
        AudioOutputDevice device;
        device.id = name;
        device.name = sink_description_from_pactl_list(details, name);
        // Controllers show up as Pulse sinks (DualSense headphone jack) — skip in UI.
        if (looks_like_controller_audio_device(device.id) ||
            looks_like_controller_audio_device(device.name)) {
            continue;
        }
        device.is_default = (name == defaults && !is_archstreamer_capture_sink(defaults));
        devices.push_back(std::move(device));
    }
#endif
    return devices;
}

std::string resolve_preferred_pulse_sink() {
    const auto preferred = preferred_audio_output_device();
    if (!preferred.empty() && preferred != "auto" &&
        !is_archstreamer_capture_sink(preferred) &&
        !looks_like_controller_audio_device(preferred)) {
        return preferred;
    }

    // Auto: never use the silent ArchStreamer null sink or a gamepad headphone jack,
    // even if PipeWire left one of those as the session default after a host session.
    const auto devices = list_pulse_output_devices();
    const auto defaults = linux_default_pulse_sink();
    if (!defaults.empty() && !is_archstreamer_capture_sink(defaults) &&
        !looks_like_controller_audio_device(defaults)) {
        for (const auto& device : devices) {
            if (device.id == defaults) {
                return defaults;
            }
        }
        // Default exists but was filtered (controller) — fall through.
        if (!looks_like_controller_audio_device(defaults) &&
            !is_archstreamer_capture_sink(defaults)) {
            return defaults;
        }
    }
    for (const auto& device : devices) {
        if (device.is_default) {
            return device.id;
        }
    }
    if (!devices.empty()) {
        return devices.front().id;
    }
    return {};
}

} // namespace

void set_preferred_audio_output_device(std::string id) {
    if (id.empty()) {
        id = "auto";
    }
    {
        std::lock_guard lock(g_preferred_mutex);
        if (g_preferred_audio_output == id) {
            return;
        }
        g_preferred_audio_output = std::move(id);
    }
    g_preferred_epoch.fetch_add(1, std::memory_order_relaxed);
}

std::string preferred_audio_output_device() {
    std::lock_guard lock(g_preferred_mutex);
    return g_preferred_audio_output;
}

std::uint64_t audio_output_preference_epoch() {
    return g_preferred_epoch.load(std::memory_order_relaxed);
}

std::vector<AudioOutputDevice> list_audio_output_devices() {
    std::vector<AudioOutputDevice> devices;
    devices.push_back(AudioOutputDevice{"auto", "Auto (system default)", true});
#ifdef _WIN32
    for (const auto& device : list_wasapi2_sink_devices()) {
        if (device.id.empty() || looks_like_controller_audio_device(device.name)) {
            continue;
        }
        if (contains_ci(device.name, "default audio render device")) {
            continue;
        }
        devices.push_back(AudioOutputDevice{device.id, device.name, device.is_default});
    }
#else
    for (auto& device : list_pulse_output_devices()) {
        if (device.is_default) {
            devices[0].name = "Auto → " + device.name;
        }
        devices.push_back(std::move(device));
    }
#endif
    return devices;
}

std::string current_audio_playback_device_key() {
#ifdef _WIN32
    if (const auto device = choose_preferred_wasapi2_device(); device.has_value()) {
        return device->id;
    }
    const auto preferred = preferred_audio_output_device();
    return preferred.empty() ? std::string("default") : preferred;
#else
    return resolve_preferred_pulse_sink();
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
#else
    const auto pulse_sink = resolve_preferred_pulse_sink();
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
