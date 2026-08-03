#ifndef _WIN32
#include "host/streaming_audio_sink.hpp"

#include "common/platform/process_utils.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace archstreamer {
namespace {

bool sink_exists(const std::string& sink_name) {
    const auto sinks = read_command_output("pactl list short sinks 2>/dev/null");
    if (sinks.empty()) {
        return false;
    }
    std::string::size_type line_start = 0;
    while (line_start < sinks.size()) {
        const auto line_end = sinks.find('\n', line_start);
        const auto line = sinks.substr(
            line_start,
            line_end == std::string::npos ? std::string::npos : line_end - line_start);
        line_start = line_end == std::string::npos ? sinks.size() : line_end + 1;

        std::string::size_type field = 0;
        std::string::size_type pos = 0;
        while (pos < line.size()) {
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
                ++pos;
            }
            if (pos >= line.size()) {
                break;
            }
            const auto end = line.find_first_of(" \t", pos);
            const auto token = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (field == 1 && token == sink_name) {
                return true;
            }
            ++field;
            if (end == std::string::npos) {
                break;
            }
            pos = end;
        }
    }
    return false;
}

std::string ensure_named_null_sink(const char* sink_name, const char* description) {
    // PipeWire-Pulse will happily load module-null-sink repeatedly under the same
    // sink_name, which floods the mixer with duplicate "ArchStreamer" devices and
    // can steal the session default (silent playback for clients on Auto).
    const auto short_list = read_command_output("pactl list short sinks 2>/dev/null");
    int existing = 0;
    if (!short_list.empty()) {
        std::string::size_type line_start = 0;
        while (line_start < short_list.size()) {
            const auto line_end = short_list.find('\n', line_start);
            const auto line = short_list.substr(
                line_start,
                line_end == std::string::npos ? std::string::npos : line_end - line_start);
            line_start = line_end == std::string::npos ? short_list.size() : line_end + 1;
            // "id\tname\t..."
            const auto tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            auto rest = line.substr(tab + 1);
            const auto tab2 = rest.find('\t');
            const auto name = tab2 == std::string::npos ? rest : rest.substr(0, tab2);
            if (name == sink_name) {
                ++existing;
            }
        }
    }

    if (existing == 0) {
        const auto module = read_command_output(
            (std::string("pactl load-module module-null-sink sink_name=") + sink_name +
             " sink_properties=device.description=\"" + description +
             "\" rate=48000 channels=2 2>/dev/null")
                .c_str());
        if (module.empty() || !sink_exists(sink_name)) {
            throw std::runtime_error(
                std::string("failed to create null sink '") + sink_name +
                "' (need pactl / module-null-sink)");
        }
    } else if (existing > 1) {
        // Keep the oldest module; drop extras so the mixer stays clean.
        const auto modules = read_command_output("pactl list modules short 2>/dev/null");
        std::vector<std::string> null_ids;
        std::string::size_type pos = 0;
        const std::string needle = std::string("sink_name=") + sink_name;
        while (pos < modules.size()) {
            const auto end = modules.find('\n', pos);
            const auto line = modules.substr(
                pos,
                end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? modules.size() : end + 1;
            if (line.find("module-null-sink") == std::string::npos ||
                line.find(needle) == std::string::npos) {
                continue;
            }
            const auto tab = line.find('\t');
            if (tab != std::string::npos) {
                null_ids.push_back(line.substr(0, tab));
            }
        }
        for (std::size_t i = 1; i < null_ids.size(); ++i) {
            (void)read_command_output(
                (std::string("pactl unload-module ") + null_ids[i] + " 2>/dev/null").c_str());
        }
        if (null_ids.size() > 1) {
            std::cout
                << "Removed " << (null_ids.size() - 1)
                << " duplicate '" << sink_name << "' null sink module(s).\n";
        }
    }

    (void)read_command_output(
        (std::string("pactl suspend-sink ") + sink_name + " 0 2>/dev/null").c_str());
    return sink_name;
}

int move_matching_sink_inputs_to(
    const char* destination_sink,
    const std::function<bool(const std::string&)>& matches) {
    const auto dump = read_command_output("pactl list sink-inputs 2>/dev/null");
    if (dump.empty()) {
        return 0;
    }

    int moved = 0;
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto next = dump.find("Sink Input #", pos + 1);
        const auto block = dump.substr(
            pos,
            next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? dump.size() : next;

        if (!matches(block)) {
            continue;
        }

        const auto hash = block.find('#');
        if (hash == std::string::npos) {
            continue;
        }
        const auto id_end = block.find_first_not_of("0123456789", hash + 1);
        const auto id = block.substr(
            hash + 1,
            (id_end == std::string::npos ? block.size() : id_end) - (hash + 1));
        if (id.empty()) {
            continue;
        }

        const auto result = read_command_output(
            (std::string("pactl move-sink-input ") + id + " " + destination_sink +
             " 2>/dev/null && echo ok")
                .c_str());
        if (result.find("ok") != std::string::npos) {
            ++moved;
        }
    }
    return moved;
}

bool block_looks_like_retroarch(const std::string& block) {
    return block.find("application.process.binary = \"retroarch\"") != std::string::npos ||
        block.find("application.name = \"RetroArch\"") != std::string::npos ||
        block.find("node.name = \"RetroArch\"") != std::string::npos;
}

bool block_has_application_id(const std::string& block, const std::string& application_id) {
    // pactl: application.id = "archstreamer-slot-0"
    const std::string needle = "application.id = \"" + application_id + "\"";
    return block.find(needle) != std::string::npos;
}

} // namespace

std::string StreamingAudioSink::slot_sink_name(int slot_index) {
    if (slot_index < 0) {
        slot_index = 0;
    }
    return std::string(kName) + "-" + std::to_string(slot_index);
}

std::string StreamingAudioSink::slot_application_id(int slot_index) {
    if (slot_index < 0) {
        slot_index = 0;
    }
    return std::string(kName) + "-slot-" + std::to_string(slot_index);
}

bool StreamingAudioSink::is_streaming_sink_name(std::string_view sink_name) {
    return sink_name == kName || sink_name.rfind("archstreamer-", 0) == 0;
}

std::string StreamingAudioSink::ensure() {
    const auto name = ensure_named_null_sink(kName, "ArchStreamer");
    // Never leave the session default on the silent capture sink — clients using
    // Auto (and desktop apps) would hear nothing while the meter still moves.
    restore_default_sink();
    return name;
}

std::string StreamingAudioSink::monitor_source() {
    return ensure() + ".monitor";
}

std::string StreamingAudioSink::ensure_slot(int slot_index) {
    const auto name = slot_sink_name(slot_index);
    const auto description = "ArchStreamer slot " + std::to_string(slot_index < 0 ? 0 : slot_index);
    ensure_named_null_sink(name.c_str(), description.c_str());
    restore_default_sink();
    return name;
}

std::string StreamingAudioSink::monitor_source_for_slot(int slot_index) {
    return ensure_slot(slot_index) + ".monitor";
}

void StreamingAudioSink::park_game_audio() {
    // Viewer RetroArch must stay on the silent null sink. PipeWire stream-restore can
    // reattach it to HDMI/USB after a prior move, which leaks game audio to speakers
    // even when Watch-local is off (Watch is the only intentional local listen path).
    try {
        ensure();
    } catch (const std::exception& error) {
        std::cerr << "Warning: could not ensure streaming audio sink: " << error.what() << '\n';
        return;
    }

    const auto moved = move_matching_sink_inputs_to(kName, block_looks_like_retroarch);
    if (moved > 0) {
        std::cout
            << "Parked " << moved
            << " RetroArch stream(s) on '" << kName
            << "' (speakers stay quiet unless Watch stream locally).\n";
    }
}

void StreamingAudioSink::park_game_audio_for_slot(int slot_index) {
    const auto sink = slot_sink_name(slot_index);
    const auto app_id = slot_application_id(slot_index);
    try {
        ensure_slot(slot_index);
    } catch (const std::exception& error) {
        std::cerr
            << "Warning: could not ensure streaming audio sink for slot " << slot_index
            << ": " << error.what() << '\n';
        return;
    }

    const auto moved = move_matching_sink_inputs_to(
        sink.c_str(),
        [&](const std::string& block) {
            return block_has_application_id(block, app_id);
        });
    if (moved > 0) {
        std::cout
            << "Parked " << moved
            << " stream(s) on '" << sink
            << "' (slot " << slot_index << ").\n";
    }
}

void StreamingAudioSink::restore_default_sink() {
    // Never leave the session default on the silent capture sink — that mutes desktop
    // audio until the user notices.
    const auto current = read_command_output("pactl get-default-sink 2>/dev/null");
    if (!is_streaming_sink_name(current)) {
        return;
    }

    const auto sinks = read_command_output("pactl list short sinks 2>/dev/null");
    std::string::size_type pos = 0;
    while (pos < sinks.size()) {
        const auto end = sinks.find('\n', pos);
        const auto line = sinks.substr(
            pos,
            end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? sinks.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        const auto first_tab = line.find('\t');
        if (first_tab == std::string::npos) {
            continue;
        }
        auto rest = line.substr(first_tab + 1);
        const auto second_tab = rest.find('\t');
        const auto name = second_tab == std::string::npos ? rest : rest.substr(0, second_tab);
        if (name.empty() || is_streaming_sink_name(name)) {
            continue;
        }
        // Prefer speakers/HDMI over DualSense headphone jacks when reclaiming default.
        if (name.find("Wireless_Controller") != std::string::npos ||
            name.find("dualsense") != std::string::npos ||
            name.find("DualShock") != std::string::npos) {
            continue;
        }
        const auto result = read_command_output(
            (std::string("pactl set-default-sink ") + name + " 2>/dev/null && echo ok").c_str());
        if (result.find("ok") != std::string::npos) {
            std::cout << "Restored default sink to '" << name << "' after streaming session.\n";
            return;
        }
    }
    // Last resort: any non-null sink (including controller) beats silence.
    pos = 0;
    while (pos < sinks.size()) {
        const auto end = sinks.find('\n', pos);
        const auto line = sinks.substr(
            pos,
            end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? sinks.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        const auto first_tab = line.find('\t');
        if (first_tab == std::string::npos) {
            continue;
        }
        auto rest = line.substr(first_tab + 1);
        const auto second_tab = rest.find('\t');
        const auto name = second_tab == std::string::npos ? rest : rest.substr(0, second_tab);
        if (name.empty() || is_streaming_sink_name(name)) {
            continue;
        }
        const auto result = read_command_output(
            (std::string("pactl set-default-sink ") + name + " 2>/dev/null && echo ok").c_str());
        if (result.find("ok") != std::string::npos) {
            std::cout << "Restored default sink to '" << name << "' after streaming session.\n";
            return;
        }
    }
}

std::string StreamingAudioSink::default_monitor_source() {
    const auto sink = read_command_output("pactl get-default-sink 2>/dev/null");
    if (sink.empty()) {
        return {};
    }
    return sink + ".monitor";
}

void StreamingAudioSink::track_emulator_process(int, int) {}
void StreamingAudioSink::untrack_emulator_process(int) {}

} // namespace archstreamer

#endif // !_WIN32 — Windows backend: windows_streaming_audio_sink.cpp
