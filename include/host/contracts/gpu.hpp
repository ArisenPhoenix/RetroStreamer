#pragma once

#include <string>

namespace archstreamer {

/**
 * Resolved render/encode bind. Hardware produces this from GpuDevice
 * enumeration; console/video consume it. Not an enumerator — no scores,
 * no device lists.
 *
 * `yuzu_vulkan_index` is filled by hardware (Yuzu’s sort, not vulkaninfo).
 */
struct GpuTarget {
    std::string id;
    std::string name;
    std::string pci_bus;
    std::string prime_provider;
    int vulkan_index = -1;
    int nvidia_index = -1;
    int yuzu_vulkan_index = -1;
};

} // namespace archstreamer
