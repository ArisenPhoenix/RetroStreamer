#include "common/client_debug_log.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

namespace archstreamer {
namespace {

std::mutex& debug_log_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::filesystem::path debug_log_path() {
    const auto dir = std::filesystem::temp_directory_path() / "archstreamer-logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "gui.log";
}

std::string timestamp_hhmmss() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    char ts[32]{};
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&time_t));
    return ts;
}

void append_line(std::string_view message) {
    std::lock_guard lock(debug_log_mutex());
    std::ofstream file(debug_log_path(), std::ios::app);
    if (!file) {
        return;
    }
    file << '[' << timestamp_hhmmss() << "] " << message << '\n';
    file.flush();
}

void append_if(std::atomic_bool& flag, std::string_view prefix, std::string_view message) {
    if (!flag.load(std::memory_order_relaxed)) {
        return;
    }
    std::string line;
    line.reserve(prefix.size() + message.size() + 2);
    line.append(prefix);
    line.append(message);
    append_line(line);
}

} // namespace

ClientDebugLogFlags& client_debug_log_flags() {
    static ClientDebugLogFlags flags;
    return flags;
}

void client_debug_log_ctrl(std::string_view message) {
    append_if(client_debug_log_flags().controls, "ctrl: ", message);
}

void client_debug_log_conn(std::string_view message) {
    append_if(client_debug_log_flags().connections, "conn: ", message);
}

void client_debug_log_video(std::string_view message) {
    append_if(client_debug_log_flags().video, "video: ", message);
}

void client_debug_log_audio(std::string_view message) {
    append_if(client_debug_log_flags().audio, "audio: ", message);
}

void client_debug_log_note(std::string_view message) {
    append_line(message);
}

std::string read_log_file_tail(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end <= 0) {
        return {};
    }
    const auto size = static_cast<std::size_t>(end);
    const auto start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    std::ostringstream ss;
    ss << in.rdbuf();
    auto text = ss.str();
    if (start > 0 && !text.empty()) {
        text.insert(0, "...(truncated)...\n");
    }
    return text;
}

} // namespace archstreamer
