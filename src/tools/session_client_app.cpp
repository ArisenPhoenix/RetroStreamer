#include "tools/cli.hpp"
#include "tools/session_client_app.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>

namespace {

std::atomic_bool stop_requested = false;

void handle_signal(int) {
    stop_requested = true;
}

} // namespace

int archstreamer::run_session_client(int argc, char** argv) {
    try {
        SessionClientCli cli(std::cout, std::cerr);
        const auto args = cli.parse(argc, argv);

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif

        const ClientApp app;
        return cli.run(
            args,
            app,
            [] {
                return stop_requested.load();
            });
    } catch (const std::exception& error) {
        std::cerr << "session_client: " << error.what() << '\n';
        return 1;
    }
}
