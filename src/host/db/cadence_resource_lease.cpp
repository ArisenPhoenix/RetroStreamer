#include "host/db/cadence_resource_lease.hpp"

#include "archstreamer/runtime_cadence/cadence.hpp"
#include "host/console/retroarch_netcmd.hpp"
#include "host/db/cadence_session_events.hpp"
#include "host/hardware/streaming_audio_sink.hpp"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace archstreamer {
namespace {

std::shared_ptr<cadence::RuntimeStore> store_or_null() {
    try {
        auto store = cadence::make_runtime_store();
        if (!store || !store->ensure_ready()) {
            return nullptr;
        }
        return store;
    } catch (...) {
        return nullptr;
    }
}

int parse_display_number(const std::string& virtual_display) {
    if (virtual_display.empty() || virtual_display.front() != ':') {
        return 99;
    }
    try {
        return std::stoi(virtual_display.substr(1));
    } catch (const std::exception&) {
        return 99;
    }
}

bool x11_display_in_use(std::string_view display) {
#if defined(_WIN32)
    (void)display;
    return false;
#else
    if (display.size() < 2 || display.front() != ':') {
        return false;
    }
    const auto number = std::string(display.substr(1));
    std::error_code ec;
    if (std::filesystem::exists("/tmp/.X11-unix/X" + number, ec) && !ec) {
        return true;
    }
    if (std::filesystem::exists("/tmp/.X" + number + "-lock", ec) && !ec) {
        return true;
    }
    return false;
#endif
}

bool is_display_type(std::string_view type) {
    return type == cadence::resource::kDisplay || type == cadence::resource::kXtestDisplay;
}

std::string format_hex_u16(std::uint16_t value) {
    std::ostringstream hex;
    hex << "0x" << std::hex << value;
    return hex.str();
}

std::uint16_t parse_u16(std::string_view text, int base) {
    return static_cast<std::uint16_t>(std::stoul(std::string(text), nullptr, base));
}

int pulse_index_from_sink(const std::string& sink) {
    const std::string prefix = std::string(StreamingAudioSink::kName) + "-";
    if (sink.rfind(prefix, 0) != 0) {
        return -1;
    }
    try {
        return std::stoi(sink.substr(prefix.size()));
    } catch (const std::exception&) {
        return -1;
    }
}

template <typename MakeName>
std::vector<std::string> prefer_then_scan(int preferred, int count, MakeName&& make_name) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(count));
    if (preferred >= 0 && preferred < count) {
        out.push_back(make_name(preferred));
    }
    for (int i = 0; i < count; ++i) {
        if (i == preferred) {
            continue;
        }
        out.push_back(make_name(i));
    }
    return out;
}

} // namespace

CadenceResourcePools cadence_resource_pools_from(const HostAppConfig& config) {
    CadenceResourcePools pools;
    pools.capture_display_base = parse_display_number(config.virtual_display);
    pools.video_port_base = config.video_port;
    pools.audio_port_base = config.audio_port;
    pools.netcmd_port_base = DefaultRetroArchNetcmdPort;
    return pools;
}

CadenceResourceLease::CadenceResourceLease(std::string session_id, std::string host_id, int slot)
    : session_id_(std::move(session_id))
    , host_id_(std::move(host_id))
    , slot_(slot) {
}

void CadenceResourceLease::record_claimed(
    std::string_view type,
    std::string_view name,
    std::string_view detail) {
    cadence::RuntimeEvent event;
    event.kind = "resource_claimed";
    event.host_id = host_id_;
    event.slot = slot_;
    event.session_id = session_id_;
    event.detail = std::string(type) + "=" + std::string(name);
    if (!detail.empty()) {
        event.detail += " " + std::string(detail);
    }
    record_cadence_event(std::move(event));
}

std::string CadenceResourceLease::try_insert(
    std::string_view type,
    std::string_view name,
    std::string_view detail) {
    auto store = store_or_null();
    if (!store) {
        throw std::runtime_error("cadence unavailable; cannot claim " + std::string(type));
    }
    cadence::ResourceClaim claim;
    claim.session_id = session_id_;
    claim.resource_type = std::string(type);
    claim.resource_name = std::string(name);
    claim.host_id = host_id_;
    claim.slot = slot_;
    claim.claimed_at = cadence::now_epoch_seconds();
    claim.detail = std::string(detail);
    if (!store->claim_resource(claim)) {
        return {};
    }
    record_claimed(type, name, detail);
    return claim.resource_name;
}

std::string CadenceResourceLease::claim_named(std::string_view type, std::string_view name) {
    if (session_id_.empty()) {
        throw std::runtime_error("cadence session is not active; cannot claim " + std::string(type));
    }
    if (type.empty() || name.empty()) {
        return {};
    }
    if (const auto already = held(type); already == name) {
        return already;
    }
    return try_insert(type, name, {});
}

std::string CadenceResourceLease::held(std::string_view type) const {
    if (session_id_.empty() || type.empty()) {
        return {};
    }
    auto store = store_or_null();
    if (!store) {
        return {};
    }
    for (const auto& claim : store->list_claims(true)) {
        if (claim.session_id == session_id_ && claim.resource_type == type) {
            return claim.resource_name;
        }
    }
    return {};
}

std::string CadenceResourceLease::claim_next(std::string_view type) {
    if (session_id_.empty()) {
        throw std::runtime_error("cadence session is not active; cannot allocate " + std::string(type));
    }
    if (const auto already = held(type); !already.empty()) {
        return already;
    }

    auto store = store_or_null();
    if (!store) {
        throw std::runtime_error("cadence unavailable; cannot allocate " + std::string(type));
    }

    std::unordered_set<std::string> taken;
    for (const auto& claim : store->list_claims(true)) {
        if (claim.resource_type == type) {
            taken.insert(claim.resource_name);
        }
    }

    const int count = pools_.max_candidates;
    const int preferred = slot_ >= 0 ? slot_ : 0;
    std::vector<std::string> candidates;

    if (type == cadence::resource::kDisplay) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return ":" + std::to_string(pools_.capture_display_base + i);
        });
    } else if (type == cadence::resource::kXtestDisplay) {
        // First free from the nest base. Do not prefer :20+slot — that is the
        // collision (gamescope lands on the next free X, not the pin).
        candidates.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            candidates.push_back(":" + std::to_string(pools_.xtest_display_base + i));
        }
    } else if (type == cadence::resource::kVideoPort) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return std::to_string(pools_.video_port_base + i * pools_.port_stride);
        });
    } else if (type == cadence::resource::kAudioPort) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return std::to_string(pools_.audio_port_base + i * pools_.port_stride);
        });
    } else if (type == cadence::resource::kNetcmdPort) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return std::to_string(pools_.netcmd_port_base + i);
        });
    } else if (type == cadence::resource::kPulseSink) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return StreamingAudioSink::slot_sink_name(i);
        });
    } else if (type == cadence::resource::kPulseAppId) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return StreamingAudioSink::slot_application_id(i);
        });
    } else if (type == cadence::resource::kPadProductBase) {
        candidates = prefer_then_scan(preferred, count, [&](int i) {
            return format_hex_u16(
                static_cast<std::uint16_t>(pools_.pad_product_base + i * pools_.pad_stride));
        });
    } else if (type == cadence::resource::kSlotLock) {
        candidates.push_back("slot-" + std::to_string(slot_));
    } else {
        throw std::runtime_error("unknown cadence resource type: " + std::string(type));
    }

    for (const auto& name : candidates) {
        if (taken.contains(name)) {
            continue;
        }
        if (is_display_type(type) && x11_display_in_use(name)) {
            continue;
        }
        if (auto claimed = try_insert(type, name, {}); !claimed.empty()) {
            return claimed;
        }
    }

    throw std::runtime_error("no free " + std::string(type) + " remaining in the session pool");
}

bool CadenceResourceLease::replace(std::string_view type, std::string_view new_name) {
    if (session_id_.empty() || type.empty() || new_name.empty()) {
        return false;
    }
    const auto current = held(type);
    if (current == new_name) {
        return true;
    }
    if (try_insert(type, new_name, "replace").empty()) {
        return false;
    }
    if (!current.empty()) {
        if (auto store = store_or_null()) {
            (void)store->release_resource(session_id_, std::string(type), current);
        }
    }
    return true;
}

void CadenceResourceLease::release_all() {
    if (session_id_.empty()) {
        return;
    }
    if (auto store = store_or_null()) {
        (void)store->release_session_resources(session_id_);
    }
}

SessionResourceGrant allocate_session_resources(
    CadenceResourceLease& lease,
    const SessionResourceNeed& need) {
    SessionResourceGrant grant;
    if (need.slot_lock) {
        grant.slot_lock = lease.claim_next(cadence::resource::kSlotLock);
    }
    if (need.capture_display) {
        grant.capture_display = lease.claim_next(cadence::resource::kDisplay);
    }
    if (need.video_port) {
        grant.video_port = parse_u16(lease.claim_next(cadence::resource::kVideoPort), 10);
    }
    if (need.audio_port) {
        grant.audio_port = parse_u16(lease.claim_next(cadence::resource::kAudioPort), 10);
    }
    if (need.netcmd_port) {
        grant.netcmd_port = parse_u16(lease.claim_next(cadence::resource::kNetcmdPort), 10);
    }
    if (need.pulse) {
        grant.pulse_sink = lease.claim_next(cadence::resource::kPulseSink);
        const int index = pulse_index_from_sink(grant.pulse_sink);
        const std::string app_id = index >= 0
            ? StreamingAudioSink::slot_application_id(index)
            : grant.pulse_sink;
        if (auto claimed = lease.held(cadence::resource::kPulseAppId); !claimed.empty()) {
            grant.pulse_app_id = claimed;
        } else if (auto inserted = lease.claim_named(cadence::resource::kPulseAppId, app_id);
                   !inserted.empty()) {
            grant.pulse_app_id = inserted;
        } else {
            grant.pulse_app_id = lease.claim_next(cadence::resource::kPulseAppId);
        }
    }
    if (need.pad_product_base) {
        grant.pad_product_base =
            parse_u16(lease.claim_next(cadence::resource::kPadProductBase), 0);
    }
    if (need.xtest_display) {
        grant.xtest_display = lease.claim_next(cadence::resource::kXtestDisplay);
    }
    return grant;
}

void apply_session_resource_grant(HostAppConfig& config, const SessionResourceGrant& grant) {
    if (!grant.capture_display.empty()) {
        config.virtual_display = grant.capture_display;
    }
    if (grant.video_port != 0) {
        config.video_port = grant.video_port;
    }
    if (grant.audio_port != 0) {
        config.audio_port = grant.audio_port;
    }
}

} // namespace archstreamer
