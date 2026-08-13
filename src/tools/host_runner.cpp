#include "tools/cli.hpp"
#include "tools/host_runner_app.hpp"

#include "common/cli_common.hpp"
#include "common/remote_host.hpp"
#include "host/hardware/gpu_select.hpp"
#include "host/host_app.hpp"

#include <csignal>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#endif

namespace {

std::atomic_bool stop_requested = false;

std::string host_log_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time_t);
#else
    localtime_r(&time_t, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

struct TimestampedLogSink {
    explicit TimestampedLogSink(const std::filesystem::path& path)
        : file(path, std::ios::trunc) {}

    std::ofstream file;
    std::mutex mutex;
};

class TimestampedLineBuffer final : public std::streambuf {
public:
    explicit TimestampedLineBuffer(std::shared_ptr<TimestampedLogSink> sink)
        : sink_(std::move(sink)) {}

    void flush_pending() {
        if (!line_.empty()) {
            flush_line();
        }
    }

protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) {
            return sync() == 0 ? traits_type::not_eof(ch) : traits_type::eof();
        }
        append_char(static_cast<char>(ch));
        return ch;
    }

    std::streamsize xsputn(const char* s, std::streamsize count) override {
        for (std::streamsize i = 0; i < count; ++i) {
            append_char(s[i]);
        }
        return count;
    }

    int sync() override {
        if (sink_ && sink_->file) {
            std::lock_guard lock(sink_->mutex);
            sink_->file.flush();
        }
        return 0;
    }

private:
    void append_char(char ch) {
        if (ch == '\n') {
            flush_line();
            return;
        }
        line_.push_back(ch);
    }

    void flush_line() {
        if (!sink_ || !sink_->file) {
            line_.clear();
            return;
        }
        if (!line_.empty() && line_.back() == '\r') {
            line_.pop_back();
        }
        std::lock_guard lock(sink_->mutex);
        sink_->file << '[' << host_log_timestamp() << "] [host] " << line_ << '\n';
        sink_->file.flush();
        line_.clear();
    }

    std::shared_ptr<TimestampedLogSink> sink_;
    std::string line_;
};

class HostLogRedirect final {
public:
    explicit HostLogRedirect(const std::filesystem::path& path)
        : sink_(std::make_shared<TimestampedLogSink>(path)),
          cout_buffer_(sink_),
          cerr_buffer_(sink_),
          old_cout_(std::cout.rdbuf(&cout_buffer_)),
          old_cerr_(std::cerr.rdbuf(&cerr_buffer_)) {}

    ~HostLogRedirect() {
        cout_buffer_.flush_pending();
        cerr_buffer_.flush_pending();
        std::cout.flush();
        std::cerr.flush();
        std::cout.rdbuf(old_cout_);
        std::cerr.rdbuf(old_cerr_);
    }

    bool is_open() const {
        return sink_ && sink_->file.is_open();
    }

private:
    std::shared_ptr<TimestampedLogSink> sink_;
    TimestampedLineBuffer cout_buffer_;
    TimestampedLineBuffer cerr_buffer_;
    std::streambuf* old_cout_ = nullptr;
    std::streambuf* old_cerr_ = nullptr;
};

std::unique_ptr<HostLogRedirect> active_host_log_redirect;

void handle_signal(int) {
    stop_requested = true;
}

bool owner_gui_alive(int pid) {
    if (pid <= 0) {
        return true;
    }
#if defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
        const auto proc = std::filesystem::path("/proc") / std::to_string(pid);
        std::error_code ec;
        auto exe = std::filesystem::read_symlink(proc / "exe", ec).filename().string();
        constexpr std::string_view deleted = " (deleted)";
        if (exe.size() > deleted.size() &&
            std::string_view(exe).substr(exe.size() - deleted.size()) == deleted) {
            exe.resize(exe.size() - deleted.size());
        }
        if (exe == "archstreamer_gui" || exe == "archstreamer_gui.exe") {
            return true;
        }
        std::ifstream cmdline(proc / "cmdline", std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
        const auto end = raw.find('\0');
        const auto argv0 = raw.substr(0, end == std::string::npos ? raw.size() : end);
        const auto base = std::filesystem::path(argv0).filename().string();
        return base == "archstreamer_gui" || base == "archstreamer_gui.exe";
    }
    return errno == EPERM;
#endif
}

bool redirect_host_log(
    const std::filesystem::path& log_root,
    std::uint16_t control_port) {
    if (log_root.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(log_root, ec);
    if (ec) {
        std::cerr << "host_runner: failed to create log directory \""
                  << log_root.string() << "\": " << ec.message() << '\n';
        return false;
    }

    const auto log_path = log_root / ("host_" + std::to_string(control_port) + ".log");
    auto redirect = std::make_unique<HostLogRedirect>(log_path);
    if (!redirect->is_open()) {
        std::cerr << "host_runner: failed to create log \""
                  << log_path.string() << "\"\n";
        return false;
    }
    active_host_log_redirect = std::move(redirect);
    std::cout << "host_runner: log " << log_path.string() << '\n';
    std::cout << "host_runner: logging to " << log_path.string() << '\n';
    return true;
}

} // namespace

int archstreamer::run_host_runner(int argc, char** argv) {
    try {
        const HostRunnerCli cli(std::cout, std::cerr);
        auto config = cli.parse(argc, argv);

        if (config.list_gpus) {
            for (const auto& device : list_render_gpus()) {
                std::cout << device.id << '\t' << device.name;
                if (device.memory_mib > 0) {
                    std::cout << " (" << device.memory_mib << " MiB)";
                }
                std::cout << '\n';
            }
            return 0;
        }

        if (!config.log_root.empty()) {
            const auto control_port = config.control_port.value_or(RemoteDefaultControlPort);
            if (!redirect_host_log(config.log_root, control_port)) {
                return 1;
            }
        }

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);

        const int owner_gui_pid = config.owner_gui_pid;
        HostApp app(std::move(config));
        return app.run([owner_gui_pid] {
            if (stop_requested.load()) {
                return true;
            }
            if (owner_gui_pid > 0 && !owner_gui_alive(owner_gui_pid)) {
                stop_requested.store(true);
                std::cout << "Owner GUI exited; stopping host.\n";
                return true;
            }
            return false;
        });
    } catch (const std::exception& error) {
        if (stop_requested.load()) {
            std::cout << "Host stopped.\n";
            return 0;
        }
        std::cerr << "host_runner: " << error.what() << '\n';
        return 1;
    }
}
