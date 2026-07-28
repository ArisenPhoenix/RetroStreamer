#include "host/session_runtime.hpp"

#include <stdexcept>

namespace archstreamer {

SharedEmulatorRuntime::SharedEmulatorRuntime(HostLaunchPlan plan)
    : launch_plan_(std::move(plan)),
      emulator_(std::make_unique<HostRetroArchProcess>()) {}

void SharedEmulatorRuntime::bind_launch_config(RetroArchLaunchConfig config) {
    launch_config_ = std::move(config);
    launch_config_bound_ = true;
}

ClientId SharedEmulatorRuntime::logical_host_client_id() const {
    if (!launch_plan_.seats.seats.empty()) {
        return launch_plan_.seats.seats.front().client_id;
    }
    return HostClientId;
}

void SharedEmulatorRuntime::start_emulator() {
    if (!launch_config_bound_) {
        throw std::runtime_error("SessionRuntime: launch config not bound before start_emulator");
    }
    if (!emulator_) {
        emulator_ = std::make_unique<HostRetroArchProcess>();
    }
    emulator_->launch(launch_config_);
}

void SharedEmulatorRuntime::stop_emulator() {
    if (emulator_) {
        emulator_->stop();
    }
}

bool SharedEmulatorRuntime::emulator_running() const {
    return emulator_ && emulator_->running();
}

std::optional<int> SharedEmulatorRuntime::last_exit_code() const {
    if (!emulator_) {
        return std::nullopt;
    }
    return emulator_->last_exit_code();
}

std::string SharedEmulatorRuntime::last_stderr_tail() const {
    if (!emulator_) {
        return {};
    }
    return emulator_->last_stderr_tail();
}

RetroArchProcess& SharedEmulatorRuntime::emulator() {
    if (!emulator_) {
        emulator_ = std::make_unique<HostRetroArchProcess>();
    }
    return *emulator_;
}

const RetroArchProcess& SharedEmulatorRuntime::emulator() const {
    if (!emulator_) {
        throw std::runtime_error("SessionRuntime: emulator not allocated");
    }
    return *emulator_;
}

std::unique_ptr<HostRetroArchProcess> SharedEmulatorRuntime::release_emulator() {
    return std::move(emulator_);
}

LinkSessionRuntime::LinkSessionRuntime(
    HostLaunchPlan plan,
    RetroArchLaunchConfig host_config,
    std::unique_ptr<HostRetroArchProcess> host_emulator,
    LinkPromotionRequest promotion)
    : launch_plan_(std::move(plan)),
      host_config_(std::move(host_config)),
      host_emulator_(std::move(host_emulator)),
      promotion_(std::move(promotion)) {
    if (!host_emulator_) {
        host_emulator_ = std::make_unique<HostRetroArchProcess>();
    }
    // Keep launch plan save username on the logical host for now.
    if (!promotion_.logical_host_username.empty()) {
        launch_plan_.save_username = promotion_.logical_host_username;
    }
    status_message_ =
        "Link runtime active: logical host=" + promotion_.logical_host_username +
        " client=" + promotion_.logical_client_username +
        " (" + promotion_.system_key +
        "). Primary emulator kept; peer instance not started yet.";
}

std::uint8_t LinkSessionRuntime::emulator_instance_count() const {
    return peer_emulator_ ? 2 : 1;
}

void LinkSessionRuntime::bind_launch_config(RetroArchLaunchConfig config) {
    host_config_ = std::move(config);
}

void LinkSessionRuntime::start_emulator() {
    if (!host_emulator_) {
        host_emulator_ = std::make_unique<HostRetroArchProcess>();
    }
    if (!host_emulator_->running()) {
        host_emulator_->launch(host_config_);
    }
    // Peer spawn is explicit via start_peer_emulator_stub() until dual media lands.
}

void LinkSessionRuntime::stop_emulator() {
    if (peer_emulator_) {
        peer_emulator_->stop();
    }
    if (host_emulator_) {
        host_emulator_->stop();
    }
}

bool LinkSessionRuntime::emulator_running() const {
    return host_emulator_ && host_emulator_->running();
}

std::optional<int> LinkSessionRuntime::last_exit_code() const {
    if (!host_emulator_) {
        return std::nullopt;
    }
    return host_emulator_->last_exit_code();
}

std::string LinkSessionRuntime::last_stderr_tail() const {
    if (!host_emulator_) {
        return {};
    }
    return host_emulator_->last_stderr_tail();
}

RetroArchProcess& LinkSessionRuntime::emulator() {
    if (!host_emulator_) {
        host_emulator_ = std::make_unique<HostRetroArchProcess>();
    }
    return *host_emulator_;
}

const RetroArchProcess& LinkSessionRuntime::emulator() const {
    if (!host_emulator_) {
        throw std::runtime_error("LinkSessionRuntime: host emulator not allocated");
    }
    return *host_emulator_;
}

bool LinkSessionRuntime::start_peer_emulator_stub() {
    peer_stub_attempted_ = true;
    // Dual capture + netpacket wiring comes next; do not spawn a second process yet
    // (it would fight for the same display/stream).
    status_message_ =
        "Link peer stub: would start second " + promotion_.system_key +
        " instance for " + promotion_.logical_client_username +
        " (dual media + link transport not wired yet)";
    return false;
}

std::unique_ptr<SessionRuntime> make_session_runtime(HostLaunchPlan plan) {
    switch (plan.session_mode) {
        case GameSessionMode::Multiplayer:
            return std::make_unique<MultiSessionRuntime>(std::move(plan));
        case GameSessionMode::SinglePlayer:
        default:
            return std::make_unique<SingleSessionRuntime>(std::move(plan));
    }
}

std::unique_ptr<LinkSessionRuntime> promote_to_link_runtime(
    std::unique_ptr<SessionRuntime> current,
    LinkPromotionRequest request) {
    if (!current) {
        return nullptr;
    }
    auto* shared = dynamic_cast<SharedEmulatorRuntime*>(current.get());
    if (shared == nullptr) {
        return nullptr;
    }

    auto plan = shared->launch_plan();
    auto config = shared->launch_config();
    auto process = shared->release_emulator();
    // Drop the old Single/Multi shell; process ownership moves into Link.
    current.reset();

    auto link = std::make_unique<LinkSessionRuntime>(
        std::move(plan),
        std::move(config),
        std::move(process),
        std::move(request));
    link->start_peer_emulator_stub();
    return link;
}

} // namespace archstreamer
