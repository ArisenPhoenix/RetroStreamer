#include "client/audio_playback_device.hpp"

#include "client/gstreamer_probe.hpp"
#include "common/platform/process_utils.hpp"

#include <cctype>
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

bool is_archstreamer_capture_sink(const std::string& name) {
    return name == "archstreamer" || name.rfind("archstreamer", 0) == 0;
}

std::string linux_default_pulse_sink() {
    return trim_copy(read_command_output("pactl get-default-sink 2>/dev/null"));
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

std::vector<AudioOutputDevice> list_audio_output_devices() {
    std::vector<AudioOutputDevice> devices;
    devices.push_back(AudioOutputDevice{"auto", "Auto (system default)", true});
    for (auto& device : list_pulse_output_devices()) {
        if (device.is_default) {
            devices[0].name = "Auto → " + device.name;
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

std::string current_audio_playback_device_key() {
    return resolve_preferred_pulse_sink();
}

AudioPlaybackSink choose_audio_playback_sink(bool sync) {
    const char* sync_flag = sync ? "true" : "false";
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
    return {
        {"autoaudiosink", std::string("sync=") + sync_flag},
        sync ? "autoaudiosink sync=true" : "autoaudiosink",
        current_audio_playback_device_key(),
    };
}

} // namespace archstreamer
