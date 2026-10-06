#pragma once

#include "host/contracts/launch.hpp"

#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

/** Event/js nodes the session process holds for children it spawns. */
struct IsolatedPadNode {
    std::string path;
    int fd = -1;
};

/** Remember a pad node opened by this process (the session / host instance). */
void register_isolated_pad_node(IsolatedPadNode node);

/** Drop nodes whose backing device is gone (after uinput destroy). */
void clear_isolated_pad_nodes_for_paths(const std::vector<std::string>& paths);

std::vector<IsolatedPadNode> isolated_pad_nodes();

/** Sibling of host_runner: libarchstreamer_pad_access.so */
std::string pad_access_library_path();

/**
 * Env for every emulator child: inherited pad fds + LD_PRELOAD so the child
 * reuses this process's open nodes instead of opening the host /dev/input leak.
 */
void contribute_isolated_pad_environment(ProcessEnvironment& env);

/**
 * In the emulator child after fork: keep only stdio + pad fds, compact them,
 * and point ARCHSTREAMER_PAD_ACCESS at the compact fds.
 */
void apply_isolated_pads_in_child();

/** sysfs → /dev/input/eventN and jsN for a uinput write fd. */
std::vector<std::string> input_dev_nodes_for_uinput(int uinput_fd);

/**
 * Open a pad node after udev has granted access, keep it across exec, and
 * EVIOCGRAB event devices so desktop GLFW/SDL clients get no events.
 */
int hold_input_node_for_session(const std::string& path);

/**
 * Stop the seat/desktop from handing this node to every process of the user.
 * Existing open fds (this process, already-running emulator) keep working.
 */
bool hide_input_node_from_desktop(const std::string& path);

} // namespace archstreamer
