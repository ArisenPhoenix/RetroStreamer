#include "host/windows_retroarch_process.hpp"

#ifdef _WIN32

#include <fstream>
#include <stdexcept>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace archstreamer {
namespace {

std::string path_string(const std::filesystem::path& path, const char* label) {
    if (path.empty()) {
        throw std::runtime_error(std::string(label) + " path is empty");
    }
    return path.string();
}

} // namespace

WindowsRetroArchProcess::~WindowsRetroArchProcess() {
    try {
        stop();
    } catch (...) {
    }
    clear_stderr_log();
}

void WindowsRetroArchProcess::clear_stderr_log() const {
    if (!stderr_log_path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(stderr_log_path_, ec);
        stderr_log_path_.clear();
    }
    last_stderr_tail_.clear();
}

void WindowsRetroArchProcess::capture_stderr_tail() const {
    last_stderr_tail_.clear();
    if (stderr_log_path_.empty() || !std::filesystem::exists(stderr_log_path_)) {
        return;
    }
    std::ifstream in(stderr_log_path_, std::ios::binary);
    if (!in) {
        return;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    constexpr std::size_t kMax = 4096;
    const auto start = size > kMax ? size - kMax : 0;
    in.seekg(static_cast<std::streamoff>(start));
    last_stderr_tail_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WindowsRetroArchProcess::refresh_exit_code() const {
    if (!launched_ || process_.running()) {
        return;
    }
    // ChildProcess closes the handle on stop; for natural exit, query before stop.
    // WindowsChildProcess::running() leaves the handle open until stop()/dtor.
}

void WindowsRetroArchProcess::launch(const RetroArchLaunchConfig& config) {
    if (running()) {
        stop();
    }
    clear_stderr_log();
    last_exit_code_.reset();

    std::vector<std::string> args;
    if (config.standalone) {
        args = config.command_prefix;
        args.push_back(path_string(config.core_path, "standalone emulator"));
        args.insert(
            args.end(),
            config.standalone_args_before_content.begin(),
            config.standalone_args_before_content.end());
        args.insert(args.end(), config.extra_args.begin(), config.extra_args.end());
        args.push_back(path_string(config.content_path, "content"));
    } else {
        args = config.command_prefix;
        if (args.empty()) {
            args.push_back(path_string(config.retroarch_path, "RetroArch"));
        }
        args.insert(args.end(), config.extra_args.begin(), config.extra_args.end());
        args.push_back("-L");
        args.push_back(path_string(config.core_path, "core"));
        args.push_back(path_string(config.content_path, "content"));
    }

    std::string stderr_path;
    if (config.quiet_stdio) {
        char temp_path[MAX_PATH]{};
        char temp_file[MAX_PATH]{};
        if (GetTempPathA(MAX_PATH, temp_path) > 0 &&
            GetTempFileNameA(temp_path, "asem", 0, temp_file) != 0) {
            stderr_path = temp_file;
            stderr_log_path_ = temp_file;
        }
    }

    process_.start(args, config.environment, {}, stderr_path);
    launched_ = true;
}

void WindowsRetroArchProcess::stop() {
    if (!launched_) {
        return;
    }
    if (process_.running()) {
        process_.stop();
        last_exit_code_ = 1;
    } else if (!last_exit_code_.has_value()) {
        last_exit_code_ = 0;
    }
    capture_stderr_tail();
    launched_ = false;
}

bool WindowsRetroArchProcess::running() const {
    if (!launched_) {
        return false;
    }
    if (process_.running()) {
        return true;
    }
    if (!last_exit_code_.has_value()) {
        last_exit_code_ = 0;
        capture_stderr_tail();
    }
    launched_ = false;
    return false;
}

std::optional<int> WindowsRetroArchProcess::last_exit_code() const {
    return last_exit_code_;
}

std::string WindowsRetroArchProcess::last_stderr_tail() const {
    return last_stderr_tail_;
}

} // namespace archstreamer

#endif // _WIN32
