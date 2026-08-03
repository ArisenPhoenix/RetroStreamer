#include "common/client_logs.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace archstreamer {
namespace {

std::string sanitize_username(std::string_view username) {
    std::string out;
    out.reserve(username.size());
    for (unsigned char ch : username) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(ch));
        } else if (ch == ' ' || ch == '.') {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        out = "client";
    }
    return out;
}

std::filesystem::path client_log_dir() {
    const auto dir = std::filesystem::temp_directory_path() / "archstreamer-logs";
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

std::string extract_last_log_sessions(
    std::string_view log_text,
    std::string_view session_marker,
    std::uint32_t session_count) {
    if (log_text.empty() || session_count == 0) {
        return {};
    }
    std::vector<std::size_t> starts;
    for (std::size_t pos = 0; pos < log_text.size();) {
        const auto found = log_text.find(session_marker, pos);
        if (found == std::string_view::npos) {
            break;
        }
        starts.push_back(found);
        pos = found + session_marker.size();
    }
    if (starts.empty()) {
        // No markers — return a tail so something useful still arrives.
        constexpr std::size_t kTail = 256 * 1024;
        if (log_text.size() <= kTail) {
            return std::string(log_text);
        }
        return std::string(log_text.substr(log_text.size() - kTail));
    }
    const std::size_t take = std::min<std::size_t>(session_count, starts.size());
    const std::size_t begin = starts[starts.size() - take];
    return std::string(log_text.substr(begin));
}

std::string extract_last_log_sessions_from_file(
    const std::filesystem::path& path,
    std::string_view session_marker,
    std::uint32_t session_count) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return extract_last_log_sessions(ss.str(), session_marker, session_count);
}

std::filesystem::path save_client_log_bundle(const ClientLogBundle& bundle) {
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&time_t));
    const auto name = "client-" + sanitize_username(bundle.username) + "-" + stamp + ".log";
    const auto path = client_log_dir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "# ArchStreamer client log bundle\n";
    out << "# username=" << bundle.username << "\n";
    out << "# session_count=" << bundle.session_count << "\n";
    out << "# bytes=" << bundle.text.size() << "\n\n";
    if (!bundle.text.empty()) {
        out.write(
            reinterpret_cast<const char*>(bundle.text.data()),
            static_cast<std::streamsize>(bundle.text.size()));
    }
    out.flush();
    return path;
}

ErrorPacket acknowledge_client_log_bundle(const ClientLogBundle& bundle) {
    const auto path = save_client_log_bundle(bundle);
    std::cout << "Saved client log bundle from " << bundle.username
              << " (" << bundle.session_count << " session(s), "
              << bundle.text.size() << " bytes) → " << path << '\n';
    return ErrorPacket{"logs saved: " + path.filename().string()};
}

} // namespace archstreamer
