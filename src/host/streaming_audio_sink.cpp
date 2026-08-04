#ifndef _WIN32
#include "host/streaming_audio_sink.hpp"
#include "host/session_audio_channel.hpp"
#include "host/launch_environment.hpp"

#include "common/platform/process_utils.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

namespace archstreamer {
namespace {

std::mutex g_track_mutex;
// slot_index -> root emulator/wrapper PID (-1 key = single-session)
std::unordered_map<int, int> g_tracked_roots;

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
    // Monitor capture includes sink soft-volume; keep it at 100% so host mixer
    // knobs on "ArchStreamer" do not change what remotes hear.
    (void)read_command_output(
        (std::string("pactl set-sink-volume ") + sink_name + " 100% 2>/dev/null").c_str());
    (void)read_command_output(
        (std::string("pactl set-sink-mute ") + sink_name + " 0 2>/dev/null").c_str());
    return sink_name;
}

[[nodiscard]] std::optional<int> parse_prop_int(const std::string& block, const char* key) {
    const std::string needle = std::string(key) + " = \"";
    const auto pos = block.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const auto start = pos + needle.size();
    const auto end = block.find('"', start);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    try {
        return std::stoi(block.substr(start, end - start));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::unordered_set<int> descendant_pids(int root) {
    std::unordered_set<int> out;
    if (root <= 0) {
        return out;
    }
    std::vector<int> queue{root};
    out.insert(root);
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const int current = queue[i];
        const auto task_dir =
            std::filesystem::path("/proc") / std::to_string(current) / "task";
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(task_dir, ec)) {
            if (ec) {
                break;
            }
            std::ifstream children(entry.path() / "children");
            if (!children) {
                continue;
            }
            int child = 0;
            while (children >> child) {
                if (out.insert(child).second) {
                    queue.push_back(child);
                }
            }
        }
    }
    return out;
}

[[nodiscard]] std::unordered_map<int, int> pulse_client_sec_pids() {
    // object.id (PipeWire) → pipewire.sec.pid (host OS pid that opened the client)
    std::unordered_map<int, int> out;
    const auto dump = read_command_output("pactl list clients 2>/dev/null");
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto next = dump.find("Client #", pos + 1);
        const auto block = dump.substr(
            pos,
            next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? dump.size() : next;
        const auto object_id = parse_prop_int(block, "object.id");
        const auto sec_pid = parse_prop_int(block, "pipewire.sec.pid");
        if (object_id.has_value() && sec_pid.has_value() && *sec_pid > 0) {
            out[*object_id] = *sec_pid;
        }
    }
    return out;
}

[[nodiscard]] int tracked_root_for_slot(int slot_index) {
    std::lock_guard lock(g_track_mutex);
    const auto it = g_tracked_roots.find(slot_index);
    return it == g_tracked_roots.end() ? 0 : it->second;
}

[[nodiscard]] bool block_belongs_to_process_tree(
    const std::string& block,
    const std::unordered_set<int>& tree,
    const std::unordered_map<int, int>& client_sec_pids) {
    if (tree.empty()) {
        return false;
    }
    if (const auto app_pid = parse_prop_int(block, "application.process.id");
        app_pid.has_value() && *app_pid > 0) {
        if (tree.contains(*app_pid)) {
            return true;
        }
        // AppImage / firejail: Pulse may report the nested Ryujinx pid while we
        // tracked gamescope — same process group still belongs to this slot.
        if (const int app_pgid = ::getpgid(*app_pid); app_pgid > 0) {
            for (const int pid : tree) {
                if (::getpgid(pid) == app_pgid) {
                    return true;
                }
            }
        }
    }
    if (const auto client_id = parse_prop_int(block, "client.id"); client_id.has_value()) {
        if (const auto it = client_sec_pids.find(*client_id); it != client_sec_pids.end()) {
            if (tree.contains(it->second)) {
                return true;
            }
            // firejail / PID namespaces: sec.pid may be the helper; same process
            // group as a tracked emulator is still this slot.
            if (const int node_pgid = ::getpgid(it->second); node_pgid > 0) {
                for (const int pid : tree) {
                    if (::getpgid(pid) == node_pgid) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/** Unload module-null-sink instances whose sink_name matches exactly. */
void unload_null_sink_modules_named(const std::string& sink_name) {
    const auto modules = read_command_output("pactl list modules short 2>/dev/null");
    const std::string needle = std::string("sink_name=") + sink_name;
    std::string::size_type pos = 0;
    int removed = 0;
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
        // Avoid matching archstreamer-10 when looking for archstreamer-1: require
        // sink_name=X as a whole token (end or followed by space/tab).
        const auto at = line.find(needle);
        if (at == std::string::npos) {
            continue;
        }
        const auto after = at + needle.size();
        if (after < line.size() && line[after] != ' ' && line[after] != '\t') {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const auto id = line.substr(0, tab);
        (void)read_command_output(
            (std::string("pactl unload-module ") + id + " 2>/dev/null").c_str());
        ++removed;
    }
    if (removed > 0) {
        std::cout << "Removed " << removed << " stale '" << sink_name << "' null sink(s).\n";
    }
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

        // Already routed to the capture sink — skip no-op moves (avoids log spam).
        if (block.find(std::string("target.object = \"") + destination_sink + "\"") !=
            std::string::npos) {
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

void StreamingAudioSink::prune_unused(int max_slots, bool keep_legacy) {
    // PipeWire null sinks persist across host restarts; without pruning the mixer
    // accumulates archstreamer-0…N from every past max-slots setting.
    if (max_slots < 0) {
        max_slots = 0;
    }
    if (max_slots > 4) {
        max_slots = 4; // matches clamp_max_session_slots upper bound
    }
    if (!keep_legacy) {
        unload_null_sink_modules_named(kName);
    }
    // Drop high slot indices and anything beyond the current budget.
    for (int slot = max_slots; slot < 16; ++slot) {
        unload_null_sink_modules_named(slot_sink_name(slot));
    }
}

std::string StreamingAudioSink::ensure() {
    prune_unused(/*max_slots=*/4, /*keep_legacy=*/true);
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
    if (slot_index < 0) {
        slot_index = 0;
    }
    // Concurrent hosts only need 0..max_slots-1; drop leftovers from older runs.
    prune_unused(/*max_slots=*/4, /*keep_legacy=*/false);
    const auto name = slot_sink_name(slot_index);
    const auto description = "ArchStreamer slot " + std::to_string(slot_index);
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

    const int root = tracked_root_for_slot(-1);
    const auto tree = descendant_pids(root);
    const auto client_secs = root > 0 ? pulse_client_sec_pids() : std::unordered_map<int, int>{};
    const auto moved = move_matching_sink_inputs_to(
        kName,
        [&](const std::string& block) {
            return block_looks_like_retroarch(block) ||
                block_belongs_to_process_tree(block, tree, client_secs);
        });
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

    const int root = tracked_root_for_slot(slot_index);
    const auto tree = descendant_pids(root);
    const auto client_secs = root > 0 ? pulse_client_sec_pids() : std::unordered_map<int, int>{};
    const auto matches = [&](const std::string& block) {
        if (block_has_application_id(block, app_id)) {
            return true;
        }
        return block_belongs_to_process_tree(block, tree, client_secs);
    };
    // Count matches before move so "already on sink" is not reported as missing.
    int matched = 0;
    {
        const auto dump = read_command_output("pactl list sink-inputs 2>/dev/null");
        std::string::size_type pos = 0;
        while (pos < dump.size()) {
            const auto next = dump.find("Sink Input #", pos + 1);
            const auto block = dump.substr(
                pos,
                next == std::string::npos ? std::string::npos : next - pos);
            pos = next == std::string::npos ? dump.size() : next;
            if (matches(block)) {
                ++matched;
            }
        }
    }
    const auto moved = move_matching_sink_inputs_to(sink.c_str(), matches);
    if (moved > 0) {
        std::cout
            << "Parked " << moved
            << " stream(s) on '" << sink
            << "' (slot " << slot_index << ").\n";
    } else if (matched == 0 && root > 0 && tree.size() > 1) {
        // Help diagnose "phone silent / host hears game": capture sink empty.
        static thread_local int warn_slot = -1;
        static thread_local int warn_count = 0;
        if (warn_slot != slot_index) {
            warn_slot = slot_index;
            warn_count = 0;
        }
        if (warn_count < 3) {
            ++warn_count;
            std::cerr
                << "Warning: no Pulse sink-input found for slot " << slot_index
                << " (owner pid " << root << ", tree " << tree.size()
                << ") — remotes may hear silence while speakers get the game\n";
            const auto dump = read_command_output("pactl list sink-inputs 2>/dev/null");
            if (dump.find("Sink Input #") == std::string::npos) {
                std::cerr << "  (no sink-inputs listed by pactl)\n";
            } else {
                std::string::size_type pos = 0;
                int listed = 0;
                while (pos < dump.size() && listed < 6) {
                    const auto next = dump.find("Sink Input #", pos + 1);
                    const auto block = dump.substr(
                        pos,
                        next == std::string::npos ? std::string::npos : next - pos);
                    pos = next == std::string::npos ? dump.size() : next;
                    if (block.find("Sink Input #") == std::string::npos) {
                        continue;
                    }
                    std::string binary = "?";
                    std::string app = "?";
                    const auto bin_key = std::string("application.process.binary = \"");
                    if (const auto b = block.find(bin_key); b != std::string::npos) {
                        const auto s = b + bin_key.size();
                        const auto e = block.find('"', s);
                        if (e != std::string::npos) {
                            binary = block.substr(s, e - s);
                        }
                    }
                    const auto app_key = std::string("application.name = \"");
                    if (const auto a = block.find(app_key); a != std::string::npos) {
                        const auto s = a + app_key.size();
                        const auto e = block.find('"', s);
                        if (e != std::string::npos) {
                            app = block.substr(s, e - s);
                        }
                    }
                    std::cerr << "  sink-input: app=" << app << " binary=" << binary << '\n';
                    ++listed;
                }
            }
        }
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

void StreamingAudioSink::track_emulator_process(int process_id, int slot_index) {
    if (process_id <= 0) {
        return;
    }
    std::lock_guard lock(g_track_mutex);
    g_tracked_roots[slot_index] = process_id;
}

void StreamingAudioSink::untrack_emulator_process(int slot_index) {
    std::lock_guard lock(g_track_mutex);
    g_tracked_roots.erase(slot_index);
}

SessionAudioChannel::SessionAudioChannel(int slot_index)
    : slot_index_(slot_index < 0 ? 0 : slot_index)
    , sink_name_(StreamingAudioSink::slot_sink_name(slot_index_))
    , application_id_(StreamingAudioSink::slot_application_id(slot_index_)) {
    const auto description = "ArchStreamer slot " + std::to_string(slot_index_);
    ensure_named_null_sink(sink_name_.c_str(), description.c_str());
    // Do not steal the session default onto the silent capture sink.
    StreamingAudioSink{}.restore_default_sink();
    sink_owned_ = true;
}

SessionAudioChannel::~SessionAudioChannel() {
    clear_emulator_pid();
    if (!sink_owned_) {
        return;
    }
    unload_null_sink_modules_named(sink_name_);
    sink_owned_ = false;
}

std::string SessionAudioChannel::monitor_source() const {
    return sink_name_ + ".monitor";
}

ProcessEnvironment SessionAudioChannel::launch_env() const {
    // Reuse the shared audio layer so PULSE_PROP / PULSE_SERVER stay consistent.
    return audio_launch_environment(
        /*stream_media=*/true,
        /*stream_audio=*/true,
        /*host_plays_locally=*/false,
        monitor_source());
}

int SessionAudioChannel::park() {
    // Recreate the null sink if prune/teardown of another host wiped it while
    // this session is still live — otherwise Pulse falls back to HDMI speakers.
    try {
        const auto description = "ArchStreamer slot " + std::to_string(slot_index_);
        ensure_named_null_sink(sink_name_.c_str(), description.c_str());
        StreamingAudioSink{}.restore_default_sink();
        sink_owned_ = true;
    } catch (const std::exception& error) {
        std::cerr
            << "Warning: could not ensure null sink '" << sink_name_
            << "' for slot " << slot_index_ << ": " << error.what() << '\n';
        return 0;
    }

    const int root = emulator_pid_ > 0
        ? emulator_pid_
        : tracked_root_for_slot(slot_index_);
    if (emulator_pid_ > 0) {
        StreamingAudioSink{}.track_emulator_process(emulator_pid_, slot_index_);
    }
    const auto tree = descendant_pids(root);
    const auto client_secs =
        root > 0 ? pulse_client_sec_pids() : std::unordered_map<int, int>{};

    const auto matches = [&](const std::string& block) {
        if (block_has_application_id(block, application_id_)) {
            return true;
        }
        // Ryujinx/Yuzu AppImages often ignore PULSE_PROP application.id — match
        // by the gamescope/emulator process tree instead (same as park_game_audio_for_slot).
        return block_belongs_to_process_tree(block, tree, client_secs);
    };

    int matched = 0;
    {
        const auto dump = read_command_output("pactl list sink-inputs 2>/dev/null");
        std::string::size_type pos = 0;
        while (pos < dump.size()) {
            const auto next = dump.find("Sink Input #", pos + 1);
            const auto block = dump.substr(
                pos,
                next == std::string::npos ? std::string::npos : next - pos);
            pos = next == std::string::npos ? dump.size() : next;
            if (matches(block)) {
                ++matched;
            }
        }
    }

    const int moved = move_matching_sink_inputs_to(sink_name_.c_str(), matches);
    if (moved > 0) {
        std::cout
            << "Parked " << moved
            << " stream(s) on '" << sink_name_
            << "' (slot " << slot_index_ << ", id " << application_id_ << ").\n";
    } else if (matched == 0) {
        static thread_local int warn_slot = -1;
        static thread_local int warn_count = 0;
        if (warn_slot != slot_index_) {
            warn_slot = slot_index_;
            warn_count = 0;
        }
        if (warn_count < 3) {
            ++warn_count;
            std::cerr
                << "Warning: no Pulse sink-input for slot " << slot_index_
                << " (id=\"" << application_id_ << "\"";
            if (root > 0) {
                std::cerr << ", owner pid " << root << ", tree " << tree.size();
            }
            std::cerr
                << ") — remotes may hear silence while speakers get the game\n";
        }
    }
    return moved;
}

void SessionAudioChannel::set_emulator_pid(int process_id) {
    emulator_pid_ = process_id > 0 ? process_id : 0;
    if (emulator_pid_ > 0) {
        StreamingAudioSink{}.track_emulator_process(emulator_pid_, slot_index_);
    } else {
        StreamingAudioSink{}.untrack_emulator_process(slot_index_);
    }
}

void SessionAudioChannel::clear_emulator_pid() {
    StreamingAudioSink{}.untrack_emulator_process(slot_index_);
    emulator_pid_ = 0;
}

} // namespace archstreamer

#endif // !_WIN32 — Windows backend: windows_streaming_audio_sink.cpp
