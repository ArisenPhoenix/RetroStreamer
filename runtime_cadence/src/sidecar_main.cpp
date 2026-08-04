#include "archstreamer/runtime_cadence/db_store.hpp"
#include "archstreamer/runtime_cadence/file_store.hpp"
#include "archstreamer/runtime_cadence/protocol.hpp"
#include "archstreamer/runtime_cadence/sidecar_db.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

std::atomic_bool g_stop{false};

void on_signal(int) {
    g_stop.store(true);
}

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " --socket <path> --db <path>\n"
        << "  Owns ArchStreamer runtime cadence SQLite (users + per-day events).\n";
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    std::cerr << "archstreamer_cadence: Unix sidecar only in Phase 1\n";
    return 1;
#else
    std::string socket_path;
    std::string db_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }
    if (socket_path.empty() || db_path.empty()) {
        // Defaults for manual runs.
        socket_path = archstreamer::cadence::default_cadence_socket_path().string();
        db_path = (archstreamer::cadence::default_cadence_data_root() / "cadence.sqlite").string();
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // Ensure signals interrupt blocking accept()/read().
    siginterrupt(SIGINT, 1);
    siginterrupt(SIGTERM, 1);

    archstreamer::cadence::SidecarDb db(db_path);
    if (!db.open()) {
        return 1;
    }

    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(socket_path).parent_path(), ec);
    ::unlink(socket_path.c_str());

    const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "cadence: socket path too long\n";
        ::close(listen_fd);
        return 1;
    }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path.c_str());
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        ::close(listen_fd);
        return 1;
    }
    ::chmod(socket_path.c_str(), 0600);
    if (::listen(listen_fd, 16) != 0) {
        std::perror("listen");
        ::close(listen_fd);
        return 1;
    }

    std::cout << "archstreamer_cadence listening on " << socket_path
              << " db=" << db_path << '\n';

    while (!g_stop.load()) {
        const int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (g_stop.load()) {
                break;
            }
            continue;
        }
        // Concurrent clients: host_started, auth, and session threads each open
        // their own store connection. Serve them in parallel.
        std::thread([client, &db]() {
            while (!g_stop.load()) {
                std::string payload;
                if (!archstreamer::cadence::read_frame(client, payload)) {
                    break;
                }
                const auto response = db.handle_request_json(payload);
                if (!archstreamer::cadence::write_frame(client, response)) {
                    break;
                }
            }
            ::close(client);
        }).detach();
    }

    ::close(listen_fd);
    ::unlink(socket_path.c_str());
    return 0;
#endif
}
