#include "host/streaming_audio_sink.hpp"

#include "common/platform/process_utils.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

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
    if (!sink_exists(sink_name)) {
        const auto module = read_command_output(
            (std::string("pactl load-module module-null-sink sink_name=") + sink_name +
             " sink_properties=device.description=\"" + description +
             "\" 2>/dev/null")
                .c_str());
        if (module.empty() || !sink_exists(sink_name)) {
            throw std::runtime_error(
                std::string("failed to create null sink '") + sink_name +
                "' (need pactl / module-null-sink)");
        }
    }
    (void)read_command_output(
        (std::string("pactl suspend-sink ") + sink_name + " 0 2>/dev/null").c_str());
    return sink_name;
}

using SinkInputMatchFn = bool (*)(const std::string&);

int move_matching_sink_inputs_to(const char* destination_sink, SinkInputMatchFn matches) {
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

} // namespace

std::string StreamingAudioSink::ensure() {
    return ensure_named_null_sink(kName, "ArchStreamer");
}

std::string StreamingAudioSink::monitor_source() {
    return ensure() + ".monitor";
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

void StreamingAudioSink::restore_default_sink() {
    // Never leave the session default on the silent capture sink — that mutes desktop
    // audio until the user notices.
    const auto current = read_command_output("pactl get-default-sink 2>/dev/null");
    if (current != kName) {
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
        if (name.empty() || name == kName) {
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

} // namespace archstreamer
