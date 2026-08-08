#include "host/emulator_orphan_reaper.hpp"

namespace archstreamer {

bool other_host_runner_alive() {
    return false;
}

// Reclaiming a leftover host_runner needs the /proc scan the Linux twin does.
// Reporting none keeps the GUI's Start Host path unchanged here.
std::vector<HostRunnerProcess> list_host_runner_processes(int) {
    return {};
}

bool terminate_host_runner(int, int) {
    return true;
}

// Windows hosts launch Yuzu directly without gamescope/setsid session trees, and
// WindowsRetroArchProcess uses job-object teardown, so there is nothing to reap.
int reap_orphaned_emulator_processes() {
    return 0;
}

void register_emulator_session_token(const std::string&) {}

void unregister_emulator_session_token(const std::string&) {}

int reap_stale_emulator_session_tokens() {
    return 0;
}

} // namespace archstreamer
