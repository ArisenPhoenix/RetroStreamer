#pragma once

#include "host/host_app_config.hpp"

#include <functional>

namespace archstreamer {

class HostApp {
public:
    explicit HostApp(HostAppConfig config);

    int run(const std::function<bool()>& should_stop);

private:
    HostAppConfig config_;
};

} // namespace archstreamer
