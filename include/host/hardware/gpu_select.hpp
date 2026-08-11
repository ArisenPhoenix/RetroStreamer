#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace archstreamer {

struct GpuDevice {
    // Stable id for settings / CLI, e.g. "auto", "nvidia:0", "nvidia:1", "mesa:amd".
    std::string id;
    std::string name;
    std::string pci_bus;
    // xrandr PRIME offload provider when proprietary NVIDIA GLX is needed (NVIDIA-G0…).
    std::string prime_provider;
    int vulkan_index = -1;
    int nvidia_index = -1;
    std::uint64_t memory_mib = 0;
    int score = 0;
};

// Enumerate GPUs usable for RetroArch on the host (NVIDIA via nvidia-smi + PRIME,
// plus Mesa AMD/Intel including iGPUs when present).
std::vector<GpuDevice> list_render_gpus();

GpuDevice preferred_render_gpu(const std::vector<GpuDevice>& devices);

/** True when selection (id or fuzzy name like "3060" / "amd") matches this device. */
bool gpu_selection_matches_device(const std::string& selection, const GpuDevice& device);

// selection: "auto" or a GpuDevice::id / fuzzy name. Returns nullopt if unknown
// and selection was explicit (non-empty, not auto).
std::optional<GpuDevice> resolve_render_gpu(const std::string& selection);

/** Resolve against an already-fetched device list (Remote Ensure Host over SSH). */
std::optional<GpuDevice> resolve_render_gpu_from(
    const std::vector<GpuDevice>& devices,
    const std::string& selection);

// Environment entries for RetroArch child (PRIME offload when NVIDIA).
std::vector<std::pair<std::string, std::string>> render_gpu_environment(const GpuDevice& gpu);

// Index into Yuzu's Vulkan device list for qt-config `vulkan_device`.
// Yuzu re-sorts physical devices (discrete first, NVIDIA before AMD, name descending),
// so this is NOT the same as GpuDevice::vulkan_index / vulkaninfo order.
int yuzu_vulkan_device_index(const GpuDevice& gpu);

} // namespace archstreamer
