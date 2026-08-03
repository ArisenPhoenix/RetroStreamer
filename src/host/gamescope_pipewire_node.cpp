#include "host/gamescope_pipewire_node.hpp"

#include "common/platform/process_utils.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <unistd.h>

namespace archstreamer {
namespace {

struct Candidate {
    int owner_score = 0;
    bool resolution_ok = false;
    bool running = false;
    int object_id = 0;
};

[[nodiscard]] std::optional<int> json_int(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int>(value.get<unsigned>());
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> read_pid_props(const nlohmann::json& props) {
    if (!props.is_object()) {
        return std::nullopt;
    }
    for (const char* key : {"pipewire.sec.pid", "application.process.id", "node.pid"}) {
        if (const auto it = props.find(key); it != props.end()) {
            if (auto pid = json_int(*it); pid.has_value() && *pid > 0) {
                return pid;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> process_group_id(int pid) {
    if (pid <= 0) {
        return std::nullopt;
    }
    const int group = ::getpgid(pid);
    if (group < 0) {
        return std::nullopt;
    }
    return group;
}

[[nodiscard]] std::optional<int> parent_pid(int pid) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    if (!status) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view prefix = "PPid:";
        if (line.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        try {
            return std::stoi(line.substr(prefix.size()));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool is_ancestor_of(int ancestor, int pid) {
    std::unordered_set<int> seen;
    int current = pid;
    for (int depth = 0; depth < 64 && current > 1 && seen.insert(current).second; ++depth) {
        if (current == ancestor) {
            return true;
        }
        const auto parent = parent_pid(current);
        if (!parent.has_value()) {
            break;
        }
        current = *parent;
    }
    return false;
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
        const auto task_dir = std::filesystem::path("/proc") / std::to_string(current) / "task";
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

[[nodiscard]] int ownership_score(
    int want_pid,
    std::optional<int> want_pgid,
    const std::unordered_set<int>& want_tree,
    std::optional<int> node_pid) {
    if (want_pid <= 0 || !node_pid.has_value()) {
        return 0;
    }
    const int pid = *node_pid;
    if (pid == want_pid || want_tree.contains(pid)) {
        return 3;
    }
    if (want_pgid.has_value()) {
        if (const auto node_pgid = process_group_id(pid);
            node_pgid.has_value() && *node_pgid == *want_pgid) {
            return 2;
        }
    }
    if (is_ancestor_of(want_pid, pid)) {
        return 1;
    }
    return 0;
}

[[nodiscard]] bool better_candidate(const Candidate& left, const Candidate& right) {
    if (left.owner_score != right.owner_score) {
        return left.owner_score < right.owner_score;
    }
    if (left.resolution_ok != right.resolution_ok) {
        return !left.resolution_ok && right.resolution_ok;
    }
    if (left.running != right.running) {
        return !left.running && right.running;
    }
    return left.object_id < right.object_id;
}

} // namespace

std::optional<std::string> select_gamescope_pipewire_node_from_dump(
    std::string_view pw_dump_json,
    int expect_width,
    int expect_height,
    int owner_pid) {
    nlohmann::json data;
    try {
        data = nlohmann::json::parse(pw_dump_json);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!data.is_array()) {
        return std::nullopt;
    }

    std::unordered_map<int, int> client_pid;
    for (const auto& object : data) {
        if (!object.is_object()) {
            continue;
        }
        const auto object_id = json_int(object.value("id", nlohmann::json()));
        if (!object_id.has_value()) {
            continue;
        }
        const auto& info = object.value("info", nlohmann::json::object());
        const auto& props = info.value("props", nlohmann::json::object());
        if (const auto pid = read_pid_props(props); pid.has_value()) {
            client_pid[*object_id] = *pid;
        }
    }

    const auto want_pgid = process_group_id(owner_pid);
    const auto want_tree = descendant_pids(owner_pid);

    std::vector<Candidate> candidates;
    for (const auto& object : data) {
        if (!object.is_object()) {
            continue;
        }
        const auto& info = object.value("info", nlohmann::json::object());
        const auto& props = info.value("props", nlohmann::json::object());
        if (props.value("media.name", "") != "gamescope" ||
            props.value("media.class", "") != "Video/Source") {
            continue;
        }
        const auto object_id = json_int(object.value("id", nlohmann::json()));
        if (!object_id.has_value()) {
            continue;
        }

        int width = 0;
        int height = 0;
        const auto& params = info.value("params", nlohmann::json::object());
        if (const auto formats = params.find("EnumFormat");
            formats != params.end() && formats->is_array()) {
            for (const auto& format : *formats) {
                const auto& size = format.value("size", nlohmann::json::object());
                const auto w = json_int(size.value("width", nlohmann::json()));
                const auto h = json_int(size.value("height", nlohmann::json()));
                if (w.has_value() && h.has_value()) {
                    width = *w;
                    height = *h;
                    break;
                }
            }
        }

        const bool resolution_ok =
            (expect_width <= 0 || expect_height <= 0) ||
            (width == expect_width && height == expect_height) ||
            (width == 0 && height == 0);

        auto pid = read_pid_props(props);
        if (!pid.has_value()) {
            if (const auto client_id = json_int(props.value("client.id", nlohmann::json()));
                client_id.has_value()) {
                if (const auto it = client_pid.find(*client_id); it != client_pid.end()) {
                    pid = it->second;
                }
            }
        }

        Candidate candidate;
        candidate.owner_score = ownership_score(owner_pid, want_pgid, want_tree, pid);
        candidate.resolution_ok = resolution_ok;
        candidate.running = info.value("state", "") == "running";
        candidate.object_id = *object_id;
        candidates.push_back(candidate);
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    auto best_owned = candidates.end();
    auto best_any = candidates.end();
    int resolution_ok_count = 0;
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (best_any == candidates.end() || better_candidate(*best_any, *it)) {
            best_any = it;
        }
        if (it->resolution_ok) {
            ++resolution_ok_count;
        }
        if (it->owner_score <= 0) {
            continue;
        }
        if (best_owned == candidates.end() || better_candidate(*best_owned, *it)) {
            best_owned = it;
        }
    }

    if (owner_pid > 0) {
        if (best_owned != candidates.end()) {
            // Prefer owned + resolution match when present.
            auto best_owned_res = candidates.end();
            for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                if (it->owner_score <= 0 || !it->resolution_ok) {
                    continue;
                }
                if (best_owned_res == candidates.end() ||
                    better_candidate(*best_owned_res, *it)) {
                    best_owned_res = it;
                }
            }
            const auto pick = best_owned_res != candidates.end() ? best_owned_res : best_owned;
            return std::to_string(pick->object_id);
        }

        // Ownership unknown: only safe when a single gamescope source exists.
        std::vector<Candidate*> pool;
        for (auto& candidate : candidates) {
            if (resolution_ok_count > 0 ? candidate.resolution_ok : true) {
                pool.push_back(&candidate);
            }
        }
        if (pool.size() == 1) {
            return std::to_string(pool.front()->object_id);
        }
        if (candidates.size() == 1) {
            return std::to_string(candidates.front().object_id);
        }
        return std::nullopt;
    }

    if (best_any == candidates.end()) {
        return std::nullopt;
    }
    // No owner filter: prefer resolution match, then score order.
    auto best_res = candidates.end();
    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
        if (!it->resolution_ok) {
            continue;
        }
        if (best_res == candidates.end() || better_candidate(*best_res, *it)) {
            best_res = it;
        }
    }
    const auto pick = best_res != candidates.end() ? best_res : best_any;
    return std::to_string(pick->object_id);
}

std::optional<std::string> wait_for_gamescope_pipewire_node(
    std::chrono::milliseconds timeout,
    int expect_width,
    int expect_height,
    int owner_pid) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto dump = read_command_output("pw-dump 2>/dev/null");
        if (auto node = select_gamescope_pipewire_node_from_dump(
                dump, expect_width, expect_height, owner_pid);
            node.has_value()) {
            return node;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return std::nullopt;
}

} // namespace archstreamer
