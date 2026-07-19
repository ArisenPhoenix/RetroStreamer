#include "tools/session_client_cli.hpp"

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

int main(int argc, char** argv) {
    try {
        archstreamer::SessionClientCli cli(std::cout, std::cerr);
        const auto args = cli.parse(argc, argv);

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);

        const archstreamer::ClientApp app;
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
