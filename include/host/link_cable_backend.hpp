#pragma once

#include "common/protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace archstreamer {

enum class LinkCableMode : std::uint8_t {
    None = 0,
    /** One RetroArch + DoubleCherryGB with 2 emulated GBs (internal cable). */
    LocalDualGb = 1,
    /** Dual gpSP + RetroArch netpacket (localhost netplay between session slots). */
    GbaNetpacket = 2,
    /** Dual Ryujinx with ldn_mitm (Local Wireless) — no mid-session relaunch. */
    SwitchLdnMitm = 3,
};

/** Queued onto an ActiveSessionSlot after a GBA Link match. */
struct GbaNetplayRelaunchRequest {
    bool is_host = false;
    std::uint16_t port = 55435;
    std::string nick;
    std::filesystem::path core_path;
};

/**
 * Host-side virtual link activation after mutual LinkCoordinator match.
 *
 * GBA cross-slot: both SP RetroArchs relaunch into RetroArch netplay (gpSP netpacket).
 * ARCHSTREAMER_DEBUG_GB_LINK: experimental DoubleCherryGB dual-machine relaunch for gb/gbc.
 */
class LinkCableBackend {
public:
    struct StartResult {
        bool ok = false;
        bool needs_relaunch = false;
        /** Host should promote SessionRuntime Single/Multi → Link. */
        bool needs_runtime_promotion = false;
        /** Cross-slot GBA: hub should queue GbaNetplayRelaunchRequest on both slots. */
        bool needs_gba_netplay = false;
        LinkCableMode mode = LinkCableMode::None;
        std::string message;
        std::filesystem::path core_path;
        std::uint16_t netplay_port = 55435;
        ClientId logical_host_client_id = 0;
        ClientId logical_client_client_id = 0;
        std::string logical_host_username;
        std::string logical_client_username;
    };

    /**
     * @param logical_host_client_id  First mutual requester (owns primary / netplay host).
     * @param logical_client_client_id Second matcher (netplay client).
     * @param peers_already_running   True when two SP session slots already have
     *        separate emulators (cross-slot Link); skips runtime promotion stub.
     * @param netplay_port_hint       Localhost TCP port for this pair (0 = default 55435).
     */
    StartResult begin(
        std::string_view system_key,
        ClientId logical_host_client_id,
        ClientId logical_client_client_id,
        std::string logical_host_username,
        std::string logical_client_username,
        std::uint8_t seated_players,
        bool peers_already_running = false,
        std::uint16_t netplay_port_hint = 0);

    void clear();

    bool active() const { return mode_ != LinkCableMode::None; }
    LinkCableMode mode() const { return mode_; }
    bool consume_relaunch_request();
    std::optional<std::filesystem::path> pending_core_path() const;
    std::uint16_t netplay_port() const { return netplay_port_; }

    ClientId client_a() const { return client_a_; }
    ClientId client_b() const { return client_b_; }
    /** Alias: first requester / logical host / RetroArch netplay host. */
    ClientId logical_host_client_id() const { return client_a_; }
    ClientId logical_client_client_id() const { return client_b_; }

    /** Write DoubleCherryGB core options for dual local machines + cable. */
    static bool write_dual_gb_core_options();
    /** Reset to a single emulated GB (normal streaming play). */
    static bool write_single_gb_core_options();

    /** Prefer DoubleCherryGB (debug GB) or gpSP (GBA link). */
    static std::optional<std::filesystem::path> resolve_link_core(std::string_view system_key);

    /** Strip prior netplay CLI flags, then append -H or -C for this role. */
    static void apply_netplay_launch_args(
        std::vector<std::string>& extra_args,
        bool is_host,
        std::uint16_t port,
        const std::string& nick);

private:
    LinkCableMode mode_ = LinkCableMode::None;
    bool relaunch_requested_ = false;
    std::filesystem::path pending_core_path_;
    std::uint16_t netplay_port_ = 55435;
    ClientId client_a_ = 0;
    ClientId client_b_ = 0;
    std::string user_a_;
    std::string user_b_;
};

} // namespace archstreamer
