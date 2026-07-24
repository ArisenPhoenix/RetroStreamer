#include "host/gpu_select.hpp"
#include "common/platform/process_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace archstreamer {
namespace {

std::string to_lower(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

int score_gpu(const GpuDevice& gpu) {
    int score = static_cast<int>(std::min<std::uint64_t>(gpu.memory_mib, 65535));
    const auto name = to_lower(gpu.name);
    if (name.find("rtx") != std::string::npos) {
        score += 50000;
    } else if (name.find("gtx") != std::string::npos || name.find("geforce") != std::string::npos) {
        score += 30000;
    }
    if (name.find("nvidia") != std::string::npos) {
        score += 10000;
    }
    if (name.find("llvmpipe") != std::string::npos || name.find("softpipe") != std::string::npos) {
        score -= 100000;
    }
    if (name.find("raphael") != std::string::npos || name.find("radeon") != std::string::npos ||
        name.find("amd") != std::string::npos) {
        // Prefer discrete cards over iGPU when VRAM-based scores are close.
        score += 1000;
    }
    return score;
}

std::vector<std::string> nvidia_prime_providers() {
    const auto dump = read_command_output("xrandr --listproviders 2>/dev/null");
    std::vector<std::string> providers;
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto line_end = dump.find('\n', pos);
        const auto line = dump.substr(
            pos,
            line_end == std::string::npos ? std::string::npos : line_end - pos);
        pos = line_end == std::string::npos ? dump.size() : line_end + 1;
        const auto marker = line.find("name:NVIDIA-G");
        if (marker == std::string::npos) {
            continue;
        }
        auto name = line.substr(marker + 5);
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) {
            name.pop_back();
        }
        if (name.rfind("NVIDIA-G", 0) == 0) {
            providers.push_back(name);
        }
    }
    return providers;
}

std::string gl_renderer_for_provider(const std::string& provider) {
    const auto command =
        "timeout 3 env __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia "
        "__NV_PRIME_RENDER_OFFLOAD_PROVIDER=" +
        provider +
        " glxinfo -B 2>/dev/null | awk -F': ' '/OpenGL renderer string/ {print $2; exit}'";
    return read_command_output(command.c_str());
}

std::vector<std::pair<std::string, int>> vulkan_device_names() {
    // vulkaninfo --summary lists deviceName lines in GPU order (index 0..N-1).
    const auto dump = read_command_output("timeout 5 vulkaninfo --summary 2>/dev/null");
    std::vector<std::pair<std::string, int>> devices;
    int index = 0;
    std::string::size_type pos = 0;
    while (pos < dump.size()) {
        const auto line_end = dump.find('\n', pos);
        const auto line = dump.substr(
            pos,
            line_end == std::string::npos ? std::string::npos : line_end - pos);
        pos = line_end == std::string::npos ? dump.size() : line_end + 1;
        const auto key = line.find("deviceName");
        if (key == std::string::npos) {
            continue;
        }
        const auto eq = line.find('=', key);
        if (eq == std::string::npos) {
            continue;
        }
        auto name = line.substr(eq + 1);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '=')) {
            name.erase(name.begin());
        }
        while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) {
            name.pop_back();
        }
        if (!name.empty()) {
            devices.emplace_back(name, index++);
        }
    }
    return devices;
}

int find_vulkan_index(
    const std::vector<std::pair<std::string, int>>& vulkan_devices,
    const std::string& name) {
    const auto needle = to_lower(name);
    for (const auto& [device_name, index] : vulkan_devices) {
        if (to_lower(device_name).find(needle) != std::string::npos ||
            needle.find(to_lower(device_name)) != std::string::npos) {
            return index;
        }
    }
    // Fallback: substring on distinctive tokens (e.g. "3060", "1660").
    for (const auto& token : {"3060", "1660", "4070", "4080", "4090", "3080", "3070"}) {
        if (needle.find(token) == std::string::npos) {
            continue;
        }
        for (const auto& [device_name, index] : vulkan_devices) {
            if (to_lower(device_name).find(token) != std::string::npos) {
                return index;
            }
        }
    }
    return -1;
}

std::string find_prime_provider(
    const std::vector<std::string>& providers,
    const std::string& name) {
    const auto needle = to_lower(name);
    for (const auto& provider : providers) {
        const auto renderer = to_lower(gl_renderer_for_provider(provider));
        if (renderer.empty()) {
            continue;
        }
        if (renderer.find(needle) != std::string::npos) {
            return provider;
        }
        for (const auto& token : {"3060", "1660", "4070", "4080", "4090", "3080", "3070"}) {
            if (needle.find(token) != std::string::npos &&
                renderer.find(token) != std::string::npos) {
                return provider;
            }
        }
    }
    return {};
}

} // namespace

std::vector<GpuDevice> list_render_gpus() {
    std::vector<GpuDevice> devices;
    const auto providers = nvidia_prime_providers();
    const auto vulkan_devices = vulkan_device_names();

    const auto nvidia_csv = read_command_output(
        "nvidia-smi --query-gpu=index,name,memory.total,pci.bus_id --format=csv,noheader,nounits 2>/dev/null");
    std::string::size_type pos = 0;
    while (pos < nvidia_csv.size()) {
        const auto line_end = nvidia_csv.find('\n', pos);
        auto line = nvidia_csv.substr(
            pos,
            line_end == std::string::npos ? std::string::npos : line_end - pos);
        pos = line_end == std::string::npos ? nvidia_csv.size() : line_end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        std::stringstream stream(line);
        std::string index_text;
        std::string name;
        std::string memory_text;
        std::string bus;
        if (!std::getline(stream, index_text, ',')) {
            continue;
        }
        if (!std::getline(stream, name, ',')) {
            continue;
        }
        if (!std::getline(stream, memory_text, ',')) {
            continue;
        }
        if (!std::getline(stream, bus)) {
            bus.clear();
        }
        auto trim = [](std::string& value) {
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                value.pop_back();
            }
        };
        trim(index_text);
        trim(name);
        trim(memory_text);
        trim(bus);
        if (name.empty()) {
            continue;
        }

        GpuDevice gpu;
        try {
            gpu.nvidia_index = std::stoi(index_text);
        } catch (const std::exception&) {
            continue;
        }
        gpu.id = "nvidia:" + std::to_string(gpu.nvidia_index);
        gpu.name = name;
        gpu.pci_bus = bus;
        try {
            gpu.memory_mib = static_cast<std::uint64_t>(std::stoull(memory_text));
        } catch (const std::exception&) {
            gpu.memory_mib = 0;
        }
        gpu.prime_provider = find_prime_provider(providers, name);
        gpu.vulkan_index = find_vulkan_index(vulkan_devices, name);
        gpu.score = score_gpu(gpu);
        devices.push_back(std::move(gpu));
    }

    // Offer the default Mesa/AMD path when Vulkan sees a non-NVIDIA, non-llvmpipe device.
    for (const auto& [device_name, index] : vulkan_devices) {
        const auto lower = to_lower(device_name);
        if (lower.find("nvidia") != std::string::npos ||
            lower.find("llvmpipe") != std::string::npos) {
            continue;
        }
        GpuDevice gpu;
        gpu.id = "mesa:" + std::to_string(index);
        gpu.name = device_name;
        gpu.vulkan_index = index;
        gpu.score = score_gpu(gpu);
        devices.push_back(std::move(gpu));
        break;
    }

    std::sort(devices.begin(), devices.end(), [](const GpuDevice& left, const GpuDevice& right) {
        return left.score > right.score;
    });
    return devices;
}

GpuDevice preferred_render_gpu(const std::vector<GpuDevice>& devices) {
    if (devices.empty()) {
        GpuDevice fallback;
        fallback.id = "auto";
        fallback.name = "Default (system)";
        return fallback;
    }
    return devices.front();
}

std::optional<GpuDevice> resolve_render_gpu(const std::string& selection) {
    const auto devices = list_render_gpus();
    if (selection.empty() || selection == "auto") {
        return preferred_render_gpu(devices);
    }
    for (const auto& device : devices) {
        if (device.id == selection) {
            return device;
        }
    }
    // Allow matching by substring of the marketing name.
    const auto needle = to_lower(selection);
    for (const auto& device : devices) {
        if (to_lower(device.name).find(needle) != std::string::npos) {
            return device;
        }
    }
    if (!devices.empty()) {
        return preferred_render_gpu(devices);
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> render_gpu_environment(const GpuDevice& gpu) {
    std::vector<std::pair<std::string, std::string>> environment;
    if (gpu.nvidia_index < 0 || gpu.prime_provider.empty()) {
        return environment;
    }
    environment.emplace_back("__NV_PRIME_RENDER_OFFLOAD", "1");
    environment.emplace_back("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
    environment.emplace_back("__NV_PRIME_RENDER_OFFLOAD_PROVIDER", gpu.prime_provider);
    environment.emplace_back("__VK_LAYER_NV_optimus", "NVIDIA_only");
    return environment;
}

} // namespace archstreamer
