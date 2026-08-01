#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace archstreamer {

std::optional<std::filesystem::path> switch_path_from_env(std::string_view key);
std::filesystem::path switch_home_dir();

void switch_copy_file_if_missing(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

void switch_copy_key_files(
    const std::filesystem::path& source_dir,
    const std::filesystem::path& destination_dir);

bool switch_directory_has_entries(const std::filesystem::path& path);

void switch_copy_directory_recursive_skip_existing(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

} // namespace archstreamer
