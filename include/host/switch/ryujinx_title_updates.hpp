#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace archstreamer {

/** Shared folder of update/DLC NSPs (Sword/Shield 1.3.2, etc.). */
std::filesystem::path switch_title_updates_directory();

/**
 * Ensure Ryujinx can see title updates/DLC:
 * - sets Config.json autoload_dirs
 * - unpacks NSP payloads into bis/user/Contents/registered (skip existing)
 */
void ensure_ryujinx_title_updates(const std::filesystem::path& ryujinx_data_root);

} // namespace archstreamer
