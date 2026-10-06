#include "host/video/gst/gst_video_worker_client.hpp"

#include "host/video/gst/gst_video_codec.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <ext/stdio_filebuf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace archstreamer {
namespace {

GstVideoEvent make_failed(std::string message) {
    return GstVideoEvent{GstVideoEventKind::Failed, std::move(message)};
}

bool is_reply(GstVideoEventKind kind) {
    return kind == GstVideoEventKind::Ready ||
        kind == GstVideoEventKind::Failed ||
        kind == GstVideoEventKind::Stopped ||
        kind == GstVideoEventKind::Status;
}

void close_inherited_fds() {
    int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
    if (max_fd < 1024) {
        max_fd = 1024;
    }
    if (max_fd > 65536) {
        max_fd = 65536;
    }
    for (int fd = 3; fd < max_fd; ++fd) {
        close(fd);
    }
}

} // namespace

class GstVideoWorkerClient::Impl {
public:
    ~Impl() {
        stop();
    }

    void start(const std::string& executable) {
        if (executable.empty()) {
            throw std::runtime_error("gst worker executable path is empty");
        }
        if (running()) {
            throw std::runtime_error("gst worker is already running");
        }

        int command_pipe[2] = {-1, -1};
        int event_pipe[2] = {-1, -1};
        if (pipe(command_pipe) != 0 || pipe(event_pipe) != 0) {
            const int error = errno;
            close_pipe(command_pipe);
            close_pipe(event_pipe);
            throw std::runtime_error(std::string("gst worker pipe failed: ") + std::strerror(error));
        }

        const pid_t child = fork();
        if (child < 0) {
            const int error = errno;
            close_pipe(command_pipe);
            close_pipe(event_pipe);
            throw std::runtime_error(std::string("gst worker fork failed: ") + std::strerror(error));
        }

        if (child == 0) {
            setpgid(0, 0);
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() == 1) {
                _exit(128);
            }
            dup2(command_pipe[0], STDIN_FILENO);
            dup2(event_pipe[1], STDOUT_FILENO);
            close_inherited_fds();
            execl(executable.c_str(), executable.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        setpgid(child, child);
        close(command_pipe[0]);
        close(event_pipe[1]);
        command_pipe[0] = -1;
        event_pipe[1] = -1;
        fcntl(command_pipe[1], F_SETFD, FD_CLOEXEC);
        fcntl(event_pipe[0], F_SETFD, FD_CLOEXEC);

        pid_ = child;
        command_fd_ = command_pipe[1];
        event_fd_ = event_pipe[0];
        closed_ = false;
        reply_ready_ = false;
        reader_ = std::thread([this] { read_events(); });
    }

    void set_event_handler(EventHandler handler) {
        std::lock_guard lock(handler_mutex_);
        handler_ = std::move(handler);
    }

    GstVideoEvent execute(const GstVideoCommand& command) {
        std::lock_guard exec_lock(execute_mutex_);
        if (!running()) {
            return make_failed("gst worker is not running");
        }

        {
            std::lock_guard lock(reply_mutex_);
            reply_ready_ = false;
            reply_.reset();
        }

        const int write_fd = dup(command_fd_);
        if (write_fd < 0) {
            return make_failed("failed to dup gst worker command pipe");
        }
        {
            // filebuf closes write_fd. command_fd_ stays open for the next command.
            __gnu_cxx::stdio_filebuf<char> buffer(write_fd, std::ios::out);
            std::ostream out(&buffer);
            if (!write_gst_video_command(out, command)) {
                return make_failed("failed to write gst worker command");
            }
        }

        std::unique_lock lock(reply_mutex_);
        const bool arrived = reply_cv_.wait_for(
            lock,
            std::chrono::seconds(30),
            [this] { return reply_ready_ || closed_; });
        if (reply_ready_ && reply_.has_value()) {
            return *reply_;
        }
        if (!arrived) {
            return make_failed("gst worker timed out");
        }
        return make_failed("gst worker exited");
    }

    void stop() {
        if (pid_ <= 0 && !reader_.joinable()) {
            return;
        }
        if (running()) {
            const auto reply = execute(gst_video_stop_command());
            (void)reply;
        }
        if (command_fd_ >= 0) {
            close(command_fd_);
            command_fd_ = -1;
        }
        if (pid_ > 0) {
            kill(-pid_, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                const pid_t result = waitpid(pid_, &status, WNOHANG);
                if (result == pid_ || (result < 0 && errno == ECHILD)) {
                    pid_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            if (pid_ > 0) {
                kill(-pid_, SIGKILL);
                waitpid(pid_, nullptr, 0);
                pid_ = -1;
            }
        }
        if (event_fd_ >= 0) {
            close(event_fd_);
            event_fd_ = -1;
        }
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    bool running() const {
        if (pid_ <= 0) {
            return false;
        }
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == 0) {
            return true;
        }
        if (result == pid_ || errno == ECHILD) {
            pid_ = -1;
            return false;
        }
        return true;
    }

    int pid() const {
        return running() ? static_cast<int>(pid_) : -1;
    }

private:
    static void close_pipe(int pipefd[2]) {
        if (pipefd[0] >= 0) {
            close(pipefd[0]);
        }
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
        }
    }

    void read_events() {
        const int read_fd = dup(event_fd_);
        if (read_fd < 0) {
            std::lock_guard lock(reply_mutex_);
            closed_ = true;
            reply_cv_.notify_one();
            return;
        }
        // filebuf closes read_fd when this function returns.
        __gnu_cxx::stdio_filebuf<char> buffer(read_fd, std::ios::in);
        std::istream in(&buffer);
        while (true) {
            GstVideoEvent event;
            if (!read_gst_video_event(in, event)) {
                break;
            }
            if (is_reply(event.kind)) {
                std::lock_guard lock(reply_mutex_);
                reply_ = std::move(event);
                reply_ready_ = true;
                reply_cv_.notify_one();
                continue;
            }
            EventHandler handler;
            {
                std::lock_guard lock(handler_mutex_);
                handler = handler_;
            }
            if (handler) {
                handler(event);
            }
        }
        std::lock_guard lock(reply_mutex_);
        closed_ = true;
        reply_cv_.notify_one();
    }

    mutable pid_t pid_ = -1;
    int command_fd_ = -1;
    int event_fd_ = -1;
    std::thread reader_;
    std::mutex execute_mutex_;
    std::mutex reply_mutex_;
    std::condition_variable reply_cv_;
    std::optional<GstVideoEvent> reply_;
    bool reply_ready_ = false;
    bool closed_ = false;
    std::mutex handler_mutex_;
    EventHandler handler_;
};

GstVideoWorkerClient::GstVideoWorkerClient() : impl_(std::make_unique<Impl>()) {}

GstVideoWorkerClient::~GstVideoWorkerClient() = default;

void GstVideoWorkerClient::start(const std::string& executable) {
    impl_->start(executable);
}

void GstVideoWorkerClient::set_event_handler(EventHandler handler) {
    impl_->set_event_handler(std::move(handler));
}

GstVideoEvent GstVideoWorkerClient::execute(const GstVideoCommand& command) {
    return impl_->execute(command);
}

void GstVideoWorkerClient::stop() {
    impl_->stop();
}

bool GstVideoWorkerClient::running() const {
    return impl_->running();
}

int GstVideoWorkerClient::pid() const {
    return impl_->pid();
}

} // namespace archstreamer
