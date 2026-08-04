#include "host/cadence_session_tracker.hpp"

#include "archstreamer/runtime_cadence/cadence.hpp"
#include "host/cadence_session_events.hpp"

#include <iomanip>
#include <sstream>

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

} // namespace

void CadenceSessionTracker::begin(
    int slot,
    std::string_view username,
    std::string_view game_key,
    std::string_view system_key,
    std::string_view mode,
    std::string_view display,
    std::uint16_t video_port,
    std::uint16_t audio_port,
    std::uint16_t netcmd_port,
    std::string_view pulse_sink,
    std::string_view pulse_app_id,
    std::uint16_t pad_product_base) {
    host_id_ = cadence_host_id();
    slot_ = slot;
    const auto started = cadence::now_epoch_seconds();
    std::ostringstream id;
    id << host_id_ << '-' << slot << '-' << started;
    session_id_ = id.str();

    auto store = store_or_null();
    if (!store) {
        session_id_.clear();
        return;
    }

    cadence::SessionRecord session;
    session.session_id = session_id_;
    session.host_id = host_id_;
    session.slot = slot;
    session.username = std::string(username);
    session.game_key = std::string(game_key);
    session.system_key = std::string(system_key);
    session.mode = std::string(mode);
    session.started_at = started;
    (void)store->upsert_session(session);

    const auto claim_one = [&](std::string_view type, std::string_view name, std::string_view detail) {
        if (name.empty()) {
            return;
        }
        cadence::ResourceClaim claim;
        claim.session_id = session_id_;
        claim.resource_type = std::string(type);
        claim.resource_name = std::string(name);
        claim.host_id = host_id_;
        claim.slot = slot_;
        claim.claimed_at = started;
        claim.detail = std::string(detail);
        (void)store->claim_resource(claim);

        cadence::RuntimeEvent event;
        event.kind = "resource_claimed";
        event.host_id = host_id_;
        event.slot = slot_;
        event.username = std::string(username);
        event.game_key = std::string(game_key);
        event.session_id = session_id_;
        event.detail = std::string(type) + "=" + std::string(name);
        if (!detail.empty()) {
            event.detail += " " + std::string(detail);
        }
        (void)store->record_event(event);
    };

    claim_one(cadence::resource::kSlotLock, "slot-" + std::to_string(slot), {});
    claim_one(cadence::resource::kDisplay, display, {});
    claim_one(cadence::resource::kVideoPort, std::to_string(video_port), {});
    claim_one(cadence::resource::kAudioPort, std::to_string(audio_port), {});
    claim_one(cadence::resource::kNetcmdPort, std::to_string(netcmd_port), {});
    claim_one(cadence::resource::kPulseSink, pulse_sink, {});
    claim_one(cadence::resource::kPulseAppId, pulse_app_id, {});
    if (pad_product_base != 0) {
        std::ostringstream hex;
        hex << "0x" << std::hex << pad_product_base;
        claim_one(cadence::resource::kPadProductBase, hex.str(), {});
    }
}

void CadenceSessionTracker::claim(
    std::string_view resource_type,
    std::string_view resource_name,
    std::string_view detail) {
    if (session_id_.empty() || resource_type.empty() || resource_name.empty()) {
        return;
    }
    auto store = store_or_null();
    if (!store) {
        return;
    }
    cadence::ResourceClaim claim;
    claim.session_id = session_id_;
    claim.resource_type = std::string(resource_type);
    claim.resource_name = std::string(resource_name);
    claim.host_id = host_id_;
    claim.slot = slot_;
    claim.detail = std::string(detail);
    (void)store->claim_resource(claim);

    cadence::RuntimeEvent event;
    event.kind = "resource_claimed";
    event.host_id = host_id_;
    event.slot = slot_;
    event.session_id = session_id_;
    event.detail = std::string(resource_type) + "=" + std::string(resource_name);
    (void)store->record_event(event);
}

void CadenceSessionTracker::claim_emulator_pid(int pid) {
    if (pid <= 0) {
        return;
    }
    claim(cadence::resource::kEmulatorPid, std::to_string(pid), {});
}

void CadenceSessionTracker::end(std::string_view end_reason) {
    if (session_id_.empty()) {
        return;
    }
    auto store = store_or_null();
    if (store) {
        (void)store->release_session_resources(session_id_);
        (void)store->end_session(session_id_, std::string(end_reason));

        cadence::RuntimeEvent event;
        event.kind = "resources_released";
        event.host_id = host_id_;
        event.slot = slot_;
        event.session_id = session_id_;
        event.detail = std::string(end_reason);
        (void)store->record_event(event);
    }
    session_id_.clear();
}

} // namespace archstreamer
