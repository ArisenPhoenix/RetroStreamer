#include "host/hardware/gpu_selection.hpp"

#include "host/hardware/gpu_select.hpp"
#include "host/video/virtual_display.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {
namespace {

std::string startup_quote(std::string value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

const char* gpu_match_kind_name(GpuSelectionMatchKind kind) {
    switch (kind) {
    case GpuSelectionMatchKind::Auto:
        return "auto";
    case GpuSelectionMatchKind::Exact:
        return "exact";
    case GpuSelectionMatchKind::Fuzzy:
        return "fuzzy";
    case GpuSelectionMatchKind::None:
    default:
        return "none";
    }
}

void log_startup_available_gpus(const std::vector<GpuDevice>& devices) {
    std::cout << "[archstreamer-startup] available-gpus=";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (i > 0) {
            std::cout << ';';
        }
        std::cout << devices[i].id << ' ' << startup_quote(devices[i].name);
    }
    std::cout << '\n';
}

void log_startup_gpu_match(
    std::string_view role,
    const std::string& requested,
    const GpuSelectionResult& result) {
    std::cout
        << "[archstreamer-startup] gpu-role=" << role
        << " gpu-request=" << startup_quote(requested.empty() ? "auto" : requested)
        << " gpu-match=" << gpu_match_kind_name(result.match_kind);
    if (result.device.has_value()) {
        std::cout
            << " selected=" << result.device->id
            << " name=" << startup_quote(result.device->name);
        if (result.device->nvidia_index >= 0) {
            std::cout << " nvidia-index=" << result.device->nvidia_index;
        }
        if (result.device->vulkan_index >= 0) {
            std::cout << " vulkan-index=" << result.device->vulkan_index;
        }
    }
    std::cout << '\n';
}

} // namespace

SessionGpuSelection resolve_session_gpu_selection(
    const GpuSettings& gpu,
    const PresentTarget& present,
    std::string_view log_prefix,
    bool detailed_logging) {
    SessionGpuSelection selection;
    const auto devices = list_render_gpus();
    const auto encode_result = resolve_render_gpu_with_match_from(devices, gpu.encode_gpu);
    const auto render_request = gpu.effective_render_gpu();
    const auto render_result = resolve_render_gpu_with_match_from(devices, render_request);
    log_startup_available_gpus(devices);
    log_startup_gpu_match("encode", gpu.encode_gpu, encode_result);
    log_startup_gpu_match("render", render_request, render_result);
    if (encode_result.device.has_value()) {
        selection.resolved_encode = gpu_target_from(*encode_result.device);
    }
    if (render_result.device.has_value()) {
        selection.resolved_gpu = gpu_target_from(*render_result.device);
    }
    if (selection.resolved_encode.has_value() && selection.resolved_encode->nvidia_index >= 0) {
        selection.nvenc_cuda_device_id = selection.resolved_encode->nvidia_index;
    }
    if (selection.resolved_gpu.has_value()) {
        const auto gpu_label = [&]() {
            return log_prefix.empty() && detailed_logging
                ? std::string("GPU: ")
                : std::string(log_prefix) + "GPU ";
        };
        const auto encode_label = [&]() {
            return log_prefix.empty() && detailed_logging
                ? std::string("Encode GPU: ")
                : std::string(log_prefix) + "encode GPU ";
        };
        const auto render_label = [&]() {
            return log_prefix.empty() && detailed_logging
                ? std::string("Render GPU: ")
                : std::string(log_prefix) + "render GPU ";
        };
        const bool same_as_encode =
            selection.resolved_encode.has_value() &&
            selection.resolved_encode->id == selection.resolved_gpu->id;
        if (same_as_encode) {
            std::cout
                << gpu_label() << selection.resolved_gpu->name
                << " [" << selection.resolved_gpu->id << "] (encode+render)";
        } else {
            if (selection.resolved_encode.has_value()) {
                std::cout
                    << encode_label() << selection.resolved_encode->name
                    << " [" << selection.resolved_encode->id << "]";
                if (detailed_logging && selection.resolved_encode->nvidia_index >= 0) {
                    std::cout << " nvidia_index=" << selection.resolved_encode->nvidia_index;
                }
                std::cout << '\n';
            }
            std::cout
                << render_label() << selection.resolved_gpu->name
                << " [" << selection.resolved_gpu->id << "]";
            if (detailed_logging && gpu.separate_render_gpu) {
                std::cout << " (separate from encode)";
            }
        }
        if (detailed_logging && selection.resolved_gpu->vulkan_index >= 0) {
            std::cout << " vulkan_index=" << selection.resolved_gpu->vulkan_index;
        }
        if (detailed_logging && !selection.resolved_gpu->prime_provider.empty()) {
            std::cout << " prime=" << selection.resolved_gpu->prime_provider;
        }
        std::cout << '\n';
        if (const auto vd = pci_vendor_device_id(selection.resolved_gpu->pci_bus); vd.has_value()) {
            selection.gamescope_vk_device = *vd;
        }
        if (detailed_logging &&
            selection.resolved_gpu->prime_provider.empty() &&
            selection.resolved_gpu->nvidia_index >= 0) {
            std::cerr
                << "Warning: NVIDIA GPU selected but no PRIME provider was mapped "
                << "(is DISPLAY set when scanning GPUs?). Capture GL may use llvmpipe.\n";
        }
        if (detailed_logging &&
            present.uses_x11_capture() && !present.uses_virtualgl() &&
            selection.resolved_gpu->nvidia_index > 0) {
            std::cerr
                << "Warning: streamed OpenGL on plain Xvfb cannot select NVIDIA GPU index "
                << selection.resolved_gpu->nvidia_index
                << " (always uses nvidia:0). Install VirtualGL (vglrun) so Host GPU works.\n";
        }
    } else if (selection.resolved_encode.has_value()) {
        const auto encode_label = log_prefix.empty() && detailed_logging
            ? std::string("Encode GPU: ")
            : std::string(log_prefix) + "encode GPU ";
        std::cout
            << encode_label << selection.resolved_encode->name
            << " [" << selection.resolved_encode->id << "]";
        if (detailed_logging && selection.resolved_encode->nvidia_index >= 0) {
            std::cout << " nvidia_index=" << selection.resolved_encode->nvidia_index;
        }
        std::cout << '\n';
    }
    if (selection.gamescope_vk_device.empty()) {
        selection.gamescope_vk_device = "10de:2504";
    }
    return selection;
}

void apply_session_gpu_to_launch_request(
    EmulatorLaunchEnvRequest& request,
    const SessionGpuSelection& selection) {
    if (selection.resolved_gpu.has_value()) {
        request.gpu_env.merge_pairs(render_gpu_environment(*selection.resolved_gpu));
    }
}

} // namespace archstreamer
