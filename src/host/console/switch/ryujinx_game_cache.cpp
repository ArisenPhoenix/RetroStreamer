#include "host/console/switch/ryujinx_game_cache.hpp"

#include "common/platform/paths.hpp"
#include "host/console/switch/default_switch_paths.hpp"
#include "host/console/switch_save_share.hpp"

#include <cstdint>
#include <iostream>
#include <system_error>

namespace archstreamer {
namespace {

std::uintmax_t directory_byte_size(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return 0;
    }
    std::uintmax_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            continue;
        }
        const auto size = entry.file_size(ec);
        if (!ec) {
            total += size;
        }
    }
    return total;
}

bool replace_directory_with(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec) || directory_byte_size(source) == 0) {
        return false;
    }
    std::filesystem::remove_all(destination, ec);
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        std::cerr << "ryujinx cache: mkdir failed " << destination << ": " << ec.message() << '\n';
        return false;
    }
    std::filesystem::copy(
        source,
        destination,
        std::filesystem::copy_options::recursive
            | std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
        std::cerr << "ryujinx cache: copy failed " << source << " → " << destination
                  << ": " << ec.message() << '\n';
        return false;
    }
    return true;
}

bool move_directory_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec)) {
        return false;
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::filesystem::remove_all(destination, ec);
    std::filesystem::rename(source, destination, ec);
    if (!ec) {
        return true;
    }
    if (!replace_directory_with(source, destination)) {
        return false;
    }
    std::filesystem::remove_all(source, ec);
    return true;
}

} // namespace

std::filesystem::path shared_ryujinx_cache_root() {
    std::error_code ec;
    const auto xdg = std::filesystem::path(archstreamer_cache_directory()) / "ryujinx";
    if (std::filesystem::is_directory(xdg, ec)) {
        return xdg;
    }
    return SwitchPaths::archstreamer_data_root() / ".cache" / "ryujinx";
}

std::filesystem::path shared_ryujinx_title_cache_directory(std::string_view title_id) {
    const auto tid = normalize_switch_title_id(title_id);
    if (tid.empty()) {
        return {};
    }
    const auto title = shared_ryujinx_cache_root() / tid;
    std::error_code ec;
    if (std::filesystem::is_directory(title / "cache", ec)) {
        return title / "cache";
    }
    return title;
}

std::filesystem::path runtime_ryujinx_title_cache_directory(
    const std::filesystem::path& ryujinx_data_root,
    std::string_view title_id) {
    const auto tid = normalize_switch_title_id(title_id);
    if (ryujinx_data_root.empty() || tid.empty()) {
        return {};
    }
    return ryujinx_data_root / "games" / tid / "cache";
}

void seed_ryujinx_game_cache_from_shared(
    const std::filesystem::path& ryujinx_data_root,
    std::string_view title_id) {
    const auto shared = shared_ryujinx_title_cache_directory(title_id);
    const auto runtime = runtime_ryujinx_title_cache_directory(ryujinx_data_root, title_id);
    if (shared.empty() || runtime.empty()) {
        return;
    }
    const auto shared_size = directory_byte_size(shared);
    if (shared_size == 0) {
        return;
    }
    const auto runtime_size = directory_byte_size(runtime);
    if (shared_size <= runtime_size) {
        return;
    }
    if (replace_directory_with(shared, runtime)) {
        std::cout
            << "ryujinx cache: seeded \"" << normalize_switch_title_id(title_id)
            << "\" from shared (" << shared_size << " > " << runtime_size << " bytes)\n";
    }
}

void merge_ryujinx_game_cache_to_shared(
    const std::filesystem::path& ryujinx_data_root,
    std::string_view title_id) {
    const auto shared = shared_ryujinx_title_cache_directory(title_id);
    const auto runtime = runtime_ryujinx_title_cache_directory(ryujinx_data_root, title_id);
    if (shared.empty() || runtime.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(runtime, ec)) {
        return;
    }
    const auto runtime_size = directory_byte_size(runtime);
    const auto shared_size = directory_byte_size(shared);
    const auto tid = normalize_switch_title_id(title_id);
    if (runtime_size > shared_size) {
        if (move_directory_replace(runtime, shared)) {
            std::cout
                << "ryujinx cache: moved \"" << tid << "\" to shared ("
                << runtime_size << " > " << shared_size << " bytes)\n";
        }
    } else {
        std::filesystem::remove_all(runtime, ec);
        std::cout
            << "ryujinx cache: dropped runtime \"" << tid << "\" ("
            << runtime_size << " <= " << shared_size << " bytes)\n";
    }
    if (std::filesystem::exists(runtime, ec)) {
        std::filesystem::remove_all(runtime, ec);
    }
}

} // namespace archstreamer
