#include "host/emulator_orphan_reaper.hpp"

namespace archstreamer {

// Windows hosts launch Yuzu directly without gamescope/setsid session trees, and
// WindowsRetroArchProcess uses job-object teardown, so there is nothing to reap.
int reap_orphaned_emulator_processes() {
    return 0;
}

} // namespace archstreamer
