#include "tools/cli.hpp"
#include "tools/host_runner_app.hpp"

#include "common/cli_common.hpp"
#include "host/hardware/gpu_select.hpp"
#include "host/host_app.hpp"

#include <csignal>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#endif

namespace {

std::atomic_bool stop_requested = false;

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
