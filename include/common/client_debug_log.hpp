#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Opt-in diagnostic categories (desktop Logs tab / Android Settings → Debug).
 * Default off — enable before a play session, then Send logs to host.
 */
struct ClientDebugLogFlags {
    std::atomic_bool controls{false};
    std::atomic_bool connections{false};
    std::atomic_bool video{false};
    std::atomic_bool audio{false};
};

ClientDebugLogFlags& client_debug_log_flags();

/** Append a `ctrl:` line when controls logging is on (file only; dedupe in callers). */
void client_debug_log_ctrl(std::string_view message);

/** Append a `conn:` line when connections logging is on. */
void client_debug_log_conn(std::string_view message);

/** Append a `video:` line when video logging is on. */
void client_debug_log_video(std::string_view message);

/** Append a `audio:` line when audio logging is on. */
void client_debug_log_audio(std::string_view message);

/** Always-on note when a category is toggled (writes even if that category is off). */
void client_debug_log_note(std::string_view message);

/** Cap a log file to its trailing bytes for bundling into Send logs. */
std::string read_log_file_tail(
    const std::filesystem::path& path,
    std::size_t max_bytes = 256 * 1024);

} // namespace archstreamer
