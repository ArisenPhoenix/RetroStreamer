#pragma once

#include "host/save_profile.hpp"
#include "host/standalone_emulator.hpp"

namespace archstreamer {

class YuzuUserProfileService {
public:
    static YuzuUserProfile prepare(
        const SaveProfile& save_profile,
        bool force_opengl = false,
        bool force_vulkan = false,
        int vulkan_device = -1,
        int resolution_scale = 1);

    static void ensure_qt_config(
        const YuzuUserProfile& profile,
        bool force_opengl,
        bool force_vulkan,
        int vulkan_device,
        int resolution_scale);
};

} // namespace archstreamer
