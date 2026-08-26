#pragma once

#include <filesystem>
#include <string_view>

namespace archstreamer {

/** Shared title caches: ~/.cache/archstreamer/ryujinx/<title_id>/cache/ */
std::filesystem::path shared_ryujinx_title_cache_directory(std::string_view title_id);

/** This session's cache: <Ryujinx>/games/<title_id>/cache/ */
std::filesystem::path runtime_ryujinx_title_cache_directory(
    const std::filesystem::path& ryujinx_data_root,
    std::string_view title_id);

/** Launch: if the shared cache for this title is larger, copy it into the user tree. */
void seed_ryujinx_game_cache_from_shared(
    const std::filesystem::path& ryujinx_data_root,
    std::string_view title_id);

/** Exit: move runtime cache to shared if it is larger, otherwise delete it. */
void merge_ryujinx_game_cache_to_shared(
    const std::filesystem::path& ryujinx_data_root,
    std::string_view title_id);

} // namespace archstreamer
