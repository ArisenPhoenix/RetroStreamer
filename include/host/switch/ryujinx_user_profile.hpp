#pragma once

#include "host/save_profile.hpp"
#include "host/standalone_emulator.hpp"

#include <string_view>

namespace archstreamer {

class RyujinxUserProfileService {
public:
    static RyujinxUserProfile prepare(
        const SaveProfile& save_profile,
        bool enable_ldn_mitm = true,
        int resolution_scale = 1,
        std::string_view profile_display_name = {},
        std::string_view lan_interface_id = {},
        std::string_view content_stem = {},
        std::string_view title_id = {});
};

} // namespace archstreamer
