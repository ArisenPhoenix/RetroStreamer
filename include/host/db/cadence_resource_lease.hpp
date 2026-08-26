#pragma once

#include "archstreamer/runtime_cadence/types.hpp"
#include "host/host_app_config.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace archstreamer {

/**
 * Host-side bases for session resource pools. Candidate generation lives only
 * in CadenceResourceLease — call sites must not do :20+slot / port math.
 */
struct CadenceResourcePools {
    int capture_display_base = 99;
    int xtest_display_base = 20;
    std::uint16_t video_port_base = 5004;
    std::uint16_t audio_port_base = 6004;
    std::uint16_t netcmd_port_base = 55355;
    std::uint16_t pad_product_base = 0xa517;
    int port_stride = 32;
    int pad_stride = 8;
    int max_candidates = 32;
};

CadenceResourcePools cadence_resource_pools_from(const HostAppConfig& config);

struct SessionResourceNeed {
    bool slot_lock = true;
    bool capture_display = true;
    bool video_port = true;
    bool audio_port = true;
    bool netcmd_port = true;
    bool pulse = false;
    bool pad_product_base = true;
    bool xtest_display = false;
};

struct SessionResourceGrant {
    std::string slot_lock;
    std::string capture_display;
    std::string xtest_display;
    std::uint16_t video_port = 0;
    std::uint16_t audio_port = 0;
    std::uint16_t netcmd_port = 0;
    std::string pulse_sink;
    std::string pulse_app_id;
    std::uint16_t pad_product_base = 0;
};

/**
 * One session's name picker. All display / port / pulse / pad / slot-lock
 * distribution goes through claim_next so inventory stays coherent.
 *
 * release_all deletes the claim rows (not a soft released_at stamp).
 */
class CadenceResourceLease {
public:
    CadenceResourceLease() = default;
    CadenceResourceLease(std::string session_id, std::string host_id, int slot);

    void set_pools(CadenceResourcePools pools) { pools_ = std::move(pools); }
    [[nodiscard]] const CadenceResourcePools& pools() const { return pools_; }

    [[nodiscard]] const std::string& session_id() const { return session_id_; }
    [[nodiscard]] const std::string& host_id() const { return host_id_; }
    [[nodiscard]] int slot() const { return slot_; }
    [[nodiscard]] bool active() const { return !session_id_.empty(); }

    /** First free name of this type. Throws if cadence is down or the pool is exhausted. */
    std::string claim_next(std::string_view type);

    /**
     * Claim this exact name. Returns empty if another session holds it.
     * Throws if cadence is down.
     */
    std::string claim_named(std::string_view type, std::string_view name);

    /** Re-pin after gamescope binds a different X display. Deletes the old row. */
    bool replace(std::string_view type, std::string_view new_name);

    [[nodiscard]] std::string held(std::string_view type) const;

    /** DELETE every claim row for this session. */
    void release_all();

private:
    std::string try_insert(std::string_view type, std::string_view name, std::string_view detail);
    void record_claimed(std::string_view type, std::string_view name, std::string_view detail);

    std::string session_id_;
    std::string host_id_;
    int slot_ = -1;
    CadenceResourcePools pools_{};
};

SessionResourceGrant allocate_session_resources(
    CadenceResourceLease& lease,
    const SessionResourceNeed& need);

void apply_session_resource_grant(HostAppConfig& config, const SessionResourceGrant& grant);

} // namespace archstreamer
