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
// plus the AMD/Mesa default path when present).
std::vector<GpuDevice> list_render_gpus();

GpuDevice preferred_render_gpu(const std::vector<GpuDevice>& devices);

// selection: "auto" or a GpuDevice::id. Returns nullopt if unknown and no fallback.
std::optional<GpuDevice> resolve_render_gpu(const std::string& selection);

// Environment entries for RetroArch child (PRIME offload when NVIDIA).
std::vector<std::pair<std::string, std::string>> render_gpu_environment(const GpuDevice& gpu);

} // namespace archstreamer
