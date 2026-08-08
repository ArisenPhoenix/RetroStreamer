#include "host/emulator_orphan_reaper.hpp"

#include "host/standalone_emulator.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <signal.h>
#include <unistd.h>

namespace archstreamer {
namespace {

constexpr const char* kSessionTokenEnv = "ARCHSTREAMER_SLOT_TOKEN";

std::mutex g_live_tokens_mutex;
std::set<std::string> g_live_tokens;

struct ProcEntry {
    pid_t pid = 0;
    pid_t ppid = 0;
    pid_t pgid = 0;
    std::string cmdline;
    std::string exe_name;
};

std::string_view strip_deleted_suffix(std::string_view name) {
    // After a rebuild, /proc/<pid>/exe often ends with " (deleted)".
    constexpr std::string_view kDeleted = " (deleted)";
    if (name.size() > kDeleted.size() &&
        name.substr(name.size() - kDeleted.size()) == kDeleted) {
        name.remove_suffix(kDeleted.size());
    }
    return name;
}

bool name_matches_owner(std::string_view name) {
    name = strip_deleted_suffix(name);
    return name == "host_runner" || name == "host_runner.exe" ||
        name == "archstreamer_gui" || name == "archstreamer_gui.exe";
}

// Binaries that own emulator trees. A tree whose ancestry reaches one of these
// is still supervised, so it must never be reaped.
bool is_owner_process(const ProcEntry& entry) {
    if (name_matches_owner(entry.exe_name)) {
        return true;
    }
    // Fall back to argv0 when /proc/exe is unreadable or a linker wrapper.
    if (entry.cmdline.empty()) {
        return false;
    }
    const auto first = entry.cmdline.substr(0, entry.cmdline.find(' '));
    return name_matches_owner(std::filesystem::path(first).filename().string());
}

bool is_host_runner(const ProcEntry& entry) {
    const auto exe = strip_deleted_suffix(entry.exe_name);
    if (exe == "host_runner" || exe == "host_runner.exe") {
        return true;
    }
    // argv0 / flatpak-spawn wrappers still mention the binary name.
    const auto first = entry.cmdline.substr(0, entry.cmdline.find(' '));
    const auto base = std::filesystem::path(first).filename().string();
    return strip_deleted_suffix(base) == "host_runner" ||
        strip_deleted_suffix(base) == "host_runner.exe";
}

bool is_gui_process(const ProcEntry& entry) {
    const auto exe = strip_deleted_suffix(entry.exe_name);
    if (exe == "archstreamer_gui" || exe == "archstreamer_gui.exe") {
        return true;
    }
    const auto first = entry.cmdline.substr(0, entry.cmdline.find(' '));
    const auto base = std::filesystem::path(first).filename().string();
    return strip_deleted_suffix(base) == "archstreamer_gui" ||
        strip_deleted_suffix(base) == "archstreamer_gui.exe";
}

/** Value following `flag` in a snapshot cmdline, or empty when absent. */
std::string cmdline_value(const std::string& cmdline, std::string_view flag) {
    // read_cmdline() joins argv with spaces; ports and GPU ids never contain one.
    std::istringstream in(cmdline);
    std::string token;
    while (in >> token) {
        if (token == flag) {
            std::string value;
            return (in >> value) ? value : std::string{};
        }
    }
    return {};
}

bool any_other_live_host(const std::vector<ProcEntry>& entries, pid_t self) {
    return std::any_of(entries.begin(), entries.end(), [self](const ProcEntry& entry) {
        return entry.pid != self && is_host_runner(entry);
    });
}

std::string read_cmdline(const std::filesystem::path& proc_dir) {
    std::ifstream in(proc_dir / "cmdline", std::ios::binary);
    if (!in) {
        return {};
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::replace(raw.begin(), raw.end(), '\0', ' ');
    return raw;
}

bool read_stat_ids(const std::filesystem::path& proc_dir, pid_t& ppid, pid_t& pgid) {
    std::ifstream in(proc_dir / "stat");
    if (!in) {
        return false;
    }
    std::string line;
    std::getline(in, line);
    // comm (field 2) is parenthesized and may contain spaces; parse after the last ')'.
    const auto close = line.rfind(')');
    if (close == std::string::npos) {
        return false;
    }
    // Remaining fields: state ppid pgrp ...
    char state = 0;
    int parsed_ppid = 0;
    int parsed_pgid = 0;
    if (std::sscanf(line.c_str() + close + 1, " %c %d %d", &state, &parsed_ppid, &parsed_pgid) != 3) {
        return false;
    }
    ppid = parsed_ppid;
    pgid = parsed_pgid;
    return true;
}

std::string read_exe_name(const std::filesystem::path& proc_dir) {
    std::error_code ec;
    const auto target = std::filesystem::read_symlink(proc_dir / "exe", ec);
    if (ec) {
        return {};
    }
    return std::string(strip_deleted_suffix(target.filename().string()));
}

std::vector<ProcEntry> snapshot_processes() {
    std::vector<ProcEntry> entries;
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec) {
            break;
        }
        const auto name = dir.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) {
            continue;
        }
        ProcEntry entry;
        entry.pid = static_cast<pid_t>(std::stol(name));
        if (!read_stat_ids(dir.path(), entry.ppid, entry.pgid)) {
            continue;
        }
        entry.cmdline = read_cmdline(dir.path());
        entry.exe_name = read_exe_name(dir.path());
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool has_live_owner_ancestor(
    pid_t pid,
    const std::map<pid_t, const ProcEntry*>& by_pid) {
    // Bound the walk: /proc is a snapshot and PID reuse could otherwise cycle.
    for (int depth = 0; depth < 64; ++depth) {
        const auto it = by_pid.find(pid);
        if (it == by_pid.end()) {
            return false;
        }
        if (is_owner_process(*it->second)) {
            return true;
        }
        pid = it->second->ppid;
        if (pid <= 1) {
            return false;
        }
    }
    return false;
}

bool process_group_has_owner(
    pid_t pgid,
    const std::vector<ProcEntry>& entries,
    const std::map<pid_t, const ProcEntry*>& by_pid) {
    for (const auto& entry : entries) {
        if (entry.pgid != pgid) {
            continue;
        }
        if (is_owner_process(entry) || has_live_owner_ancestor(entry.ppid, by_pid)) {
            return true;
        }
    }
    return false;
}

void collect_descendants(
    pid_t root,
    const std::map<pid_t, std::vector<pid_t>>& children,
    std::set<pid_t>& out) {
    if (!out.insert(root).second) {
        return;
    }
    const auto it = children.find(root);
    if (it == children.end()) {
        return;
    }
    for (const pid_t child : it->second) {
        collect_descendants(child, children, out);
    }
}

bool any_alive(const std::set<pid_t>& pids) {
    return std::any_of(pids.begin(), pids.end(), [](pid_t pid) {
        return kill(pid, 0) == 0;
    });
}

std::string read_environ_token(const std::filesystem::path& environ_path) {
    std::ifstream in(environ_path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string prefix = std::string(kSessionTokenEnv) + "=";
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto end = raw.find('\0', pos);
        const auto entry = raw.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (entry.size() > prefix.size() && entry.compare(0, prefix.size(), prefix) == 0) {
            return entry.substr(prefix.size());
        }
        if (end == std::string::npos) {
            break;
        }
        pos = end + 1;
    }
    return {};
}

} // namespace

bool other_host_runner_alive() {
    return any_other_live_host(snapshot_processes(), getpid());
}

std::vector<HostRunnerProcess> list_host_runner_processes(int ignore_pid) {
    const auto self = getpid();
    const auto entries = snapshot_processes();
    std::map<pid_t, const ProcEntry*> by_pid;
    for (const auto& entry : entries) {
        by_pid.emplace(entry.pid, &entry);
    }

    // Bounded walk: /proc is a snapshot, so PID reuse could otherwise cycle.
    const auto gui_ancestor_of = [&by_pid](pid_t pid) -> int {
        for (int depth = 0; depth < 64 && pid > 1; ++depth) {
            const auto it = by_pid.find(pid);
            if (it == by_pid.end()) {
                return 0;
            }
            if (is_gui_process(*it->second)) {
                return static_cast<int>(it->second->pid);
            }
            pid = it->second->ppid;
        }
        return 0;
    };

    std::vector<HostRunnerProcess> found;
    for (const auto& entry : entries) {
        if (entry.pid == self || entry.pid == ignore_pid || !is_host_runner(entry)) {
            continue;
        }
        HostRunnerProcess proc;
        proc.pid = static_cast<int>(entry.pid);
        proc.owner_gui_pid = gui_ancestor_of(entry.ppid);
        const auto port = cmdline_value(entry.cmdline, "--control-port");
        if (!port.empty()) {
            char* end = nullptr;
            const auto parsed = std::strtol(port.c_str(), &end, 10);
            if (end != port.c_str() && parsed > 0 && parsed <= 65535) {
                proc.control_port = static_cast<int>(parsed);
            }
        }
        proc.gpu = cmdline_value(entry.cmdline, "--gpu");
        found.push_back(std::move(proc));
    }
    return found;
}

bool terminate_host_runner(int pid, int grace_ms) {
    const auto target = static_cast<pid_t>(pid);
    if (target <= 1 || kill(target, 0) != 0) {
        return true;
    }
    kill(target, SIGTERM);
    // host_runner only notices SIGTERM when its run loop next polls the stop
    // flag, so poll for the exit rather than assuming it was immediate.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(grace_ms, 0));
    while (std::chrono::steady_clock::now() < deadline) {
        if (kill(target, 0) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(target, SIGKILL);
    for (int i = 0; i < 20; ++i) {
        if (kill(target, 0) != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return kill(target, 0) != 0;
}

int reap_orphaned_emulator_processes() {
    const auto data_root = default_archstreamer_data_root().string();
    if (data_root.empty()) {
        return 0;
    }

    const auto entries = snapshot_processes();
    const pid_t self = getpid();
    const pid_t self_group = getpgid(0);

    // Another host is already supervising sessions (e.g. a second GUI was started
    // while a child is still playing). Never touch their trees.
    if (any_other_live_host(entries, self)) {
        std::cout
            << "Skipping orphan emulator reap: another host_runner is already running.\n";
        return 0;
    }

    std::map<pid_t, const ProcEntry*> by_pid;
    std::map<pid_t, std::vector<pid_t>> children;
    for (const auto& entry : entries) {
        by_pid.emplace(entry.pid, &entry);
        children[entry.ppid].push_back(entry.pid);
    }

    std::vector<const ProcEntry*> orphan_roots;
    for (const auto& entry : entries) {
        if (entry.pid <= 1 || entry.pid == self || entry.pgid == self_group) {
            continue;
        }
        if (is_owner_process(entry)) {
            continue;
        }
        // Managed gamescope / Ryujinx / Yuzu / RetroArch all reference the
        // ArchStreamer data root on their command line.
        if (entry.cmdline.find(data_root) == std::string::npos) {
            continue;
        }
        // Only reap roots: a matching child is covered by its parent's subtree.
        if (by_pid.count(entry.ppid) != 0 &&
            by_pid.at(entry.ppid)->cmdline.find(data_root) != std::string::npos) {
            continue;
        }
        if (has_live_owner_ancestor(entry.ppid, by_pid)) {
            continue;
        }
        // AppImage / nested runtimes may re-parent to init while still sharing a
        // process group with a host-owned gamescope wrapper.
        if (process_group_has_owner(entry.pgid, entries, by_pid)) {
            continue;
        }
        orphan_roots.push_back(&entry);
    }

    if (orphan_roots.empty()) {
        return 0;
    }

    std::set<pid_t> doomed;
    for (const auto* root : orphan_roots) {
        collect_descendants(root->pid, children, doomed);
    }
    doomed.erase(self);
    doomed.erase(self_group);
    doomed.erase(0);
    doomed.erase(1);

    for (const auto* root : orphan_roots) {
        std::cout << "Reaping orphaned emulator tree (pid " << root->pid
                  << ", no live host): " << root->cmdline << '\n';
    }

    std::set<pid_t> groups;
    for (const pid_t pid : doomed) {
        const auto it = by_pid.find(pid);
        groups.insert(it != by_pid.end() ? it->second->pgid : pid);
    }
    groups.erase(self_group);

    for (const pid_t group : groups) {
        kill(-group, SIGTERM);
    }
    for (const pid_t pid : doomed) {
        kill(pid, SIGTERM);
    }

    for (int i = 0; i < 40 && any_alive(doomed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (any_alive(doomed)) {
        for (const pid_t group : groups) {
            kill(-group, SIGKILL);
        }
        for (const pid_t pid : doomed) {
            kill(pid, SIGKILL);
        }
    }

    return static_cast<int>(orphan_roots.size());
}

void register_emulator_session_token(const std::string& token) {
    if (token.empty()) {
        return;
    }
    std::lock_guard lock(g_live_tokens_mutex);
    g_live_tokens.insert(token);
}

void unregister_emulator_session_token(const std::string& token) {
    if (token.empty()) {
        return;
    }
    std::lock_guard lock(g_live_tokens_mutex);
    g_live_tokens.erase(token);
}

int reap_stale_emulator_session_tokens() {
    std::set<std::string> live;
    {
        std::lock_guard lock(g_live_tokens_mutex);
        live = g_live_tokens;
    }

    const auto entries = snapshot_processes();
    const pid_t self = getpid();
    std::map<pid_t, const ProcEntry*> by_pid;
    std::map<pid_t, std::vector<pid_t>> children;
    for (const auto& entry : entries) {
        by_pid.emplace(entry.pid, &entry);
        children[entry.ppid].push_back(entry.pid);
    }

    std::set<pid_t> doomed;
    std::set<std::string> stale_tokens;
    for (const auto& entry : entries) {
        if (entry.pid <= 1 || entry.pid == self) {
            continue;
        }
        const auto token = read_environ_token(
            std::filesystem::path("/proc") / std::to_string(entry.pid) / "environ");
        if (token.empty() || live.count(token) != 0) {
            continue;
        }
        // Still attached to a live host owner? Leave it (registration race).
        if (has_live_owner_ancestor(entry.ppid, by_pid) || is_owner_process(entry)) {
            continue;
        }
        stale_tokens.insert(token);
        collect_descendants(entry.pid, children, doomed);
    }
    doomed.erase(self);
    doomed.erase(0);
    doomed.erase(1);
    if (doomed.empty()) {
        return 0;
    }

    for (const auto& token : stale_tokens) {
        std::cout << "Reaping stale emulator session token=" << token << '\n';
    }
    for (const pid_t pid : doomed) {
        kill(pid, SIGTERM);
    }
    for (int i = 0; i < 40 && any_alive(doomed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (any_alive(doomed)) {
        for (const pid_t pid : doomed) {
            kill(pid, SIGKILL);
        }
    }
    return static_cast<int>(stale_tokens.size());
}

} // namespace archstreamer
