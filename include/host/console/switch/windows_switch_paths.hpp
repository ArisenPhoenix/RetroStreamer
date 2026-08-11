#pragma once

#include <filesystem>
#include <vector>

namespace archstreamer {

class WindowsSwitchPaths {
public:
    static std::filesystem::path archstreamer_data_root();
    static std::filesystem::path yuzu_runtime_root();
    static std::filesystem::path ryujinx_runtime_root();

    static std::vector<std::filesystem::path> keys_source_candidates();
    static std::vector<std::filesystem::path> firmware_source_candidates(
        const std::filesystem::path& managed_registered);
    static std::vector<std::filesystem::path> profiles_template_source_candidates();

    /** Copy managed firmware into the per-user Ryujinx bis tree. */
    static void bind_ryujinx_firmware(
        const std::filesystem::path& managed_registered,
        const std::filesystem::path& profile_registered);
};

} // namespace archstreamer
