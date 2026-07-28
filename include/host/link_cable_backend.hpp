#pragma once

#include "common/protocol.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace archstreamer {

enum class LinkCableMode : std::uint8_t {
    None = 0,
    /** One RetroArch + DoubleCherryGB with 2 emulated GBs (internal cable). */
    LocalDualGb = 1,
    /** Reserved: dual gpSP + localhost netpacket (not launched yet). */
    GbaNetpacket = 2,
};

/**
 * Host-side virtual link activation after mutual LinkCoordinator match.
 *
 * Default builds: matchmaking only for gba / nds / switch (multi-instance backend TBD).
 * ARCHSTREAMER_DEBUG_GB_LINK: experimental DoubleCherryGB dual-machine relaunch for gb/gbc.
 */
class LinkCableBackend {
public:
    struct StartResult {
        bool ok = false;
        bool needs_relaunch = false;
        /** Host should promote SessionRuntime Single/Multi → Link. */
        bool needs_runtime_promotion = false;
        LinkCableMode mode = LinkCableMode::None;
        std::string message;
        std::filesystem::path core_path;
        ClientId logical_host_client_id = 0;
        ClientId logical_client_client_id = 0;
        std::string logical_host_username;
        std::string logical_client_username;
    };

    /**
     * @param logical_host_client_id  First mutual requester (owns primary instance).
     * @param logical_client_client_id Second matcher (will own peer instance).
     */
    StartResult begin(
        std::string_view system_key,
        ClientId logical_host_client_id,
        ClientId logical_client_client_id,
        std::string logical_host_username,
        std::string logical_client_username,
        std::uint8_t seated_players);

    void clear();

    bool active() const { return mode_ != LinkCableMode::None; }
    LinkCableMode mode() const { return mode_; }
    bool consume_relaunch_request();
    std::optional<std::filesystem::path> pending_core_path() const;

    ClientId client_a() const { return client_a_; }
    ClientId client_b() const { return client_b_; }
    /** Alias: first requester / logical host. */
    ClientId logical_host_client_id() const { return client_a_; }
    ClientId logical_client_client_id() const { return client_b_; }

    /** Write DoubleCherryGB core options for dual local machines + cable. */
    static bool write_dual_gb_core_options();
    /** Reset to a single emulated GB (normal streaming play). */
    static bool write_single_gb_core_options();

    /** Resolve preferred core for a link session (DoubleCherryGB / gpSP). */
    static std::optional<std::filesystem::path> resolve_link_core(std::string_view system_key);

private:
    LinkCableMode mode_ = LinkCableMode::None;
    bool relaunch_requested_ = false;
    std::filesystem::path pending_core_path_;
    ClientId client_a_ = 0;
    ClientId client_b_ = 0;
    std::string user_a_;
    std::string user_b_;
};

} // namespace archstreamer
