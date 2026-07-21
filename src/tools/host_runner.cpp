#include "tools/cli.hpp"
#include "tools/host_runner_app.hpp"

#include "common/cli_common.hpp"
#include "host/host_app.hpp"

#include <csignal>
#include <atomic>
#include <iostream>

namespace {

std::atomic_bool stop_requested = false;

void handle_signal(int) {
    stop_requested = true;
}

} // namespace

int archstreamer::run_host_runner(int argc, char** argv) {
    try {
        const HostRunnerCli cli(std::cout, std::cerr);
        auto config = cli.parse(argc, argv);

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        std::signal(SIGPIPE, SIG_IGN);

        HostApp app(std::move(config));
        return app.run([] {
            return stop_requested.load();
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
