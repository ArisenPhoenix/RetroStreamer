#pragma once

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace archstreamer {

struct CoreChoice {
    std::string system_name;
    std::string core_name;
    std::filesystem::path core_path;
};

class LibretroCoreRegistry {
public:
    static LibretroCoreRegistry defaults();
    static LibretroCoreRegistry ubuntu_defaults();

    explicit LibretroCoreRegistry(std::vector<std::filesystem::path> core_dirs);

    static std::vector<std::filesystem::path> default_core_dirs();

    std::optional<CoreChoice> system_core(std::string system_key) const;
    std::optional<CoreChoice> find_for_content(const std::filesystem::path& content_path) const;

private:
    struct CoreCandidate {
        std::string key;
        std::string display_name;
    };

    struct SystemEntry {
        std::string display_name;
        std::vector<CoreCandidate> cores;
    };

    void add_system(std::string key, std::string display_name, std::initializer_list<CoreCandidate> cores);
    void add_extension_systems(std::initializer_list<std::string_view> extensions, std::string system_key);
    std::optional<std::filesystem::path> find_core_file(const std::string& core_key) const;
    static std::string normalize_extension(std::string extension);
    static std::string normalize_token(std::string token);

    std::vector<std::filesystem::path> core_dirs_;
    std::unordered_map<std::string, SystemEntry> systems_;
    std::unordered_map<std::string, std::string> extension_to_system_;
};

} // namespace archstreamer
