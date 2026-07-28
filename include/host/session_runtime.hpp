#pragma once

#include "common/protocol.hpp"
#include "host/host_launch_planner.hpp"
#include "host/platform/default_host_platform.hpp"
#include "host/retroarch_process.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace archstreamer {

/** Concrete play topologies the host session loop dispatches through. */
enum class SessionRuntimeKind : std::uint8_t {
    Single = 0, // one shared emulator, one primary player
    Multi = 1,  // one shared emulator, N pads / seats
    Link = 2,   // N emulator instances + link transport (peer stubbed for now)
};

/**
 * Virtual session runtime: owns emulator process lifetime.
 *
 * Single/Multi share one process. Link owns a logical-host process (promoted from
 * the prior shared runtime) and will own a second process for the logical client.
 */
class SessionRuntime {
public:
    virtual ~SessionRuntime() = default;

    virtual SessionRuntimeKind kind() const = 0;
    virtual const char* kind_name() const = 0;

    /** True when all seated players share one emulator process (Single/Multi). */
    virtual bool uses_shared_emulator() const = 0;
    /** How many emulator processes this runtime owns/expects. */
    virtual std::uint8_t emulator_instance_count() const = 0;

    virtual const HostLaunchPlan& launch_plan() const = 0;
    virtual HostLaunchPlan& launch_plan() = 0;

    virtual const RetroArchLaunchConfig& launch_config() const = 0;
    virtual RetroArchLaunchConfig& launch_config() = 0;
    virtual void bind_launch_config(RetroArchLaunchConfig config) = 0;

    /** Save / logical-owner username for this runtime. */
    virtual const std::string& save_username() const = 0;

    /**
     * Client that "owns" the runtime for promotion/link purposes.
     * Shared: first seated client. Link: mutual-match first requester.
     */
    virtual ClientId logical_host_client_id() const = 0;

    virtual void start_emulator() = 0;
    virtual void stop_emulator() = 0;
    virtual bool emulator_running() const = 0;
    virtual std::optional<int> last_exit_code() const = 0;
    virtual std::string last_stderr_tail() const = 0;

    /** Primary (logical-host) emulator process. */
    virtual RetroArchProcess& emulator() = 0;
    virtual const RetroArchProcess& emulator() const = 0;
};

/** Inputs to promote a shared Single/Multi runtime into Link. */
struct LinkPromotionRequest {
    ClientId logical_host_client_id = 0;
    ClientId logical_client_client_id = 0;
    std::string logical_host_username;
    std::string logical_client_username;
    std::string system_key;
};

/**
 * Shared one-process runtime used by Single and Multi.
 * Link is a sibling type that takes ownership of the primary process on promote.
 */
class SharedEmulatorRuntime : public SessionRuntime {
public:
    explicit SharedEmulatorRuntime(HostLaunchPlan plan);

    bool uses_shared_emulator() const override { return true; }
    std::uint8_t emulator_instance_count() const override { return 1; }

    const HostLaunchPlan& launch_plan() const override { return launch_plan_; }
    HostLaunchPlan& launch_plan() override { return launch_plan_; }

    const RetroArchLaunchConfig& launch_config() const override { return launch_config_; }
    RetroArchLaunchConfig& launch_config() override { return launch_config_; }
    void bind_launch_config(RetroArchLaunchConfig config) override;

    const std::string& save_username() const override { return launch_plan_.save_username; }
    ClientId logical_host_client_id() const override;

    void start_emulator() override;
    void stop_emulator() override;
    bool emulator_running() const override;
    std::optional<int> last_exit_code() const override;
    std::string last_stderr_tail() const override;

    RetroArchProcess& emulator() override;
    const RetroArchProcess& emulator() const override;

    /** Move the live process out for Link promotion (may be null if never started). */
    std::unique_ptr<HostRetroArchProcess> release_emulator();

protected:
    HostLaunchPlan launch_plan_;
    RetroArchLaunchConfig launch_config_{};
    std::unique_ptr<HostRetroArchProcess> emulator_;
    bool launch_config_bound_ = false;
};

class SingleSessionRuntime final : public SharedEmulatorRuntime {
public:
    using SharedEmulatorRuntime::SharedEmulatorRuntime;
    SessionRuntimeKind kind() const override { return SessionRuntimeKind::Single; }
    const char* kind_name() const override { return "Single"; }
};

class MultiSessionRuntime final : public SharedEmulatorRuntime {
public:
    using SharedEmulatorRuntime::SharedEmulatorRuntime;
    SessionRuntimeKind kind() const override { return SessionRuntimeKind::Multi; }
    const char* kind_name() const override { return "Multi"; }
};

/**
 * Multi-instance link runtime. Phase 1: keeps the promoted primary emulator as the
 * logical host; logical-client process + dual media/netpacket are stubbed.
 */
class LinkSessionRuntime final : public SessionRuntime {
public:
    LinkSessionRuntime(
        HostLaunchPlan plan,
        RetroArchLaunchConfig host_config,
        std::unique_ptr<HostRetroArchProcess> host_emulator,
        LinkPromotionRequest promotion);

    SessionRuntimeKind kind() const override { return SessionRuntimeKind::Link; }
    const char* kind_name() const override { return "Link"; }

    bool uses_shared_emulator() const override { return false; }
    std::uint8_t emulator_instance_count() const override;

    const HostLaunchPlan& launch_plan() const override { return launch_plan_; }
    HostLaunchPlan& launch_plan() override { return launch_plan_; }

    const RetroArchLaunchConfig& launch_config() const override { return host_config_; }
    RetroArchLaunchConfig& launch_config() override { return host_config_; }
    void bind_launch_config(RetroArchLaunchConfig config) override;

    const std::string& save_username() const override { return promotion_.logical_host_username; }
    ClientId logical_host_client_id() const override { return promotion_.logical_host_client_id; }
    ClientId logical_client_client_id() const { return promotion_.logical_client_client_id; }
    const LinkPromotionRequest& promotion() const { return promotion_; }

    /** Human-readable phase (promoted / peer stub / etc.). */
    const std::string& status_message() const { return status_message_; }

    void start_emulator() override;
    void stop_emulator() override;
    bool emulator_running() const override;
    std::optional<int> last_exit_code() const override;
    std::string last_stderr_tail() const override;

    RetroArchProcess& emulator() override;
    const RetroArchProcess& emulator() const override;

    /** Attempt to start the logical-client instance (stub until media/transport). */
    bool start_peer_emulator_stub();

private:
    HostLaunchPlan launch_plan_;
    RetroArchLaunchConfig host_config_{};
    RetroArchLaunchConfig peer_config_{};
    std::unique_ptr<HostRetroArchProcess> host_emulator_;
    std::unique_ptr<HostRetroArchProcess> peer_emulator_;
    LinkPromotionRequest promotion_{};
    std::string status_message_;
    bool peer_stub_attempted_ = false;
};

/** Build Single or Multi from the launch plan's session_mode. */
std::unique_ptr<SessionRuntime> make_session_runtime(HostLaunchPlan plan);

/**
 * Promote a Single/Multi runtime into Link, moving the live primary emulator.
 * Returns nullptr if `current` is not a shared runtime.
 */
std::unique_ptr<LinkSessionRuntime> promote_to_link_runtime(
    std::unique_ptr<SessionRuntime> current,
    LinkPromotionRequest request);

} // namespace archstreamer
