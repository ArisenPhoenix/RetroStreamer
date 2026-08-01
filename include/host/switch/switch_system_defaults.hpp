#pragma once

#include <filesystem>
#include <optional>

namespace archstreamer {

class SwitchSystemDefaults {
public:
    static std::filesystem::path system_root();
    static std::filesystem::path keys_directory();
    static std::filesystem::path managed_firmware_registered();

    /** Ensure ~/.local/share/archstreamer/system/switch/{keys,firmware} is populated. */
    static void ensure();

    static std::optional<std::filesystem::path> find_source_keys_dir();

    /** Shared firmware NCAs under ArchStreamer system/switch (seeded once). */
    static std::filesystem::path ensure_managed_firmware();

    /** Ensure the per-user Ryujinx profile can see system firmware. */
    static void ensure_ryujinx_firmware(const std::filesystem::path& data_root);
};

} // namespace archstreamer
