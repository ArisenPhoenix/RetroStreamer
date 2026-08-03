#include "host/switch/ryujinx_title_updates.hpp"

#include "host/switch/switch_system_defaults.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace archstreamer {
namespace {

constexpr std::uint32_t read_le_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t read_le_u64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(read_le_u32(p)) |
        (static_cast<std::uint64_t>(read_le_u32(p + 4)) << 32);
}

bool extract_pfs0_nsp(
    const std::filesystem::path& nsp_path,
    const std::filesystem::path& destination_dir) {
    std::ifstream in(nsp_path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::uint8_t header[16];
    in.read(reinterpret_cast<char*>(header), 16);
    if (!in || std::memcmp(header, "PFS0", 4) != 0) {
        return false;
    }
    const auto file_count = read_le_u32(header + 4);
    const auto string_table_size = read_le_u32(header + 8);
    if (file_count == 0 || file_count > 512) {
        return false;
    }

    std::vector<std::uint8_t> entries(static_cast<std::size_t>(file_count) * 24);
    in.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entries.size()));
    if (!in) {
        return false;
    }
    std::vector<char> string_table(string_table_size);
    if (string_table_size > 0) {
        in.read(string_table.data(), static_cast<std::streamsize>(string_table_size));
        if (!in) {
            return false;
        }
    }

    const auto data_start =
        16 + static_cast<std::uint64_t>(file_count) * 24 + string_table_size;
    std::filesystem::create_directories(destination_dir);

    int extracted = 0;
    for (std::uint32_t i = 0; i < file_count; ++i) {
        const auto* entry = entries.data() + static_cast<std::size_t>(i) * 24;
        const auto offset = read_le_u64(entry);
        const auto size = read_le_u64(entry + 8);
        const auto name_offset = read_le_u32(entry + 16);
        if (name_offset >= string_table_size) {
            continue;
        }
        std::string name(string_table.data() + name_offset);
        if (name.empty()) {
            continue;
        }
        const auto dest = destination_dir / name;
        if (std::filesystem::is_regular_file(dest) &&
            std::filesystem::file_size(dest) == size) {
            continue;
        }
        in.clear();
        in.seekg(static_cast<std::streamoff>(data_start + offset));
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            continue;
        }
        std::vector<char> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(size, 1 << 20)));
        std::uint64_t remaining = size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!in) {
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            remaining -= chunk;
        }
        if (remaining == 0) {
            ++extracted;
        }
    }
    return extracted > 0 || file_count > 0;
}

} // namespace

std::filesystem::path switch_title_updates_directory() {
    if (const char* env = std::getenv("ARCHSTREAMER_SWITCH_UPDATES");
        env != nullptr && *env != '\0') {
        return env;
    }
    // Prefer the shared Gaming tree when present; fall back to ArchStreamer system.
    const std::filesystem::path gaming{"/mnt/Internal_SSD/Gaming/ROMS/SwitchUpdates"};
    if (std::filesystem::is_directory(gaming)) {
        return gaming;
    }
    return SwitchSystemDefaults::system_root() / "updates";
}

void ensure_ryujinx_title_updates(const std::filesystem::path& ryujinx_data_root) {
    const auto updates_dir = switch_title_updates_directory();
    if (!std::filesystem::is_directory(updates_dir)) {
        return;
    }

    const auto registered = ryujinx_data_root / "bis" / "user" / "Contents" / "registered";
    std::filesystem::create_directories(registered);

    int nsp_count = 0;
    int unpacked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(updates_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext != ".nsp" && ext != ".NSP") {
            continue;
        }
        ++nsp_count;
        if (extract_pfs0_nsp(entry.path(), registered)) {
            ++unpacked;
        }
    }
    if (nsp_count > 0) {
        std::cout
            << "Ryujinx title updates: scanned " << nsp_count << " NSP(s) from " << updates_dir
            << " → " << registered << " (" << unpacked << " unpacked/linked)\n";
    }
}

} // namespace archstreamer
