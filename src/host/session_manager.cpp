#include "host/session_manager.hpp"

#include "host/emulator_orphan_reaper.hpp"
#include "host/host_launch_planner.hpp"
#include "host/host_session_helpers.hpp"
#include "host/session_lobby.hpp"
#include "host/session_slot_lease.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace archstreamer {
namespace {

constexpr int kSlotLeaseSpan = 16;

} // namespace

SessionManager::SessionManager(Config config)
    : config_(std::move(config)) {
    if (config_.hub == nullptr) {
        throw std::runtime_error("SessionManager requires HostSessionHub");
    }
    if (config_.catalog == nullptr) {
        throw std::runtime_error("SessionManager requires GameCatalog");
    }
    if (config_.input_demux == nullptr) {
        throw std::runtime_error("SessionManager requires InputRouterDemux");
    }
    if (config_.streaming_audio == nullptr) {
        throw std::runtime_error("SessionManager requires StreamingAudioSink");
    }
}

SessionManager::~SessionManager() {
    request_stop_all();
    join_all();
}

SessionId SessionManager::make_session_id() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<std::uint64_t> seq{1};
    return "sess-" + std::to_string(now) + "-" + std::to_string(seq.fetch_add(1));
}

void SessionManager::apply_commands(std::vector<LobbyCommand> commands) {
    for (auto& command : commands) {
        try {
            apply_one(command);
        } catch (const std::exception& error) {
            std::cerr << "SessionManager command failed: " << error.what() << '\n';
            if (command.kind == LobbyCommand::Kind::StartSession &&
                command.plan.has_value()) {
                send_error_to_session_clients(*command.plan, error.what());
            }
        }
    }
}

void SessionManager::apply_one(LobbyCommand& command) {
    switch (command.kind) {
    case LobbyCommand::Kind::StartSession:
        if (!command.plan.has_value()) {
            throw std::runtime_error("StartSession missing plan");
        }
        start_session(
            std::move(*command.plan),
            command.session_id.empty() ? make_session_id() : std::move(command.session_id));
        break;
    case LobbyCommand::Kind::EnqueueJoin: {
        if (!command.stream.has_value() || !command.hello.has_value()) {
            throw std::runtime_error("EnqueueJoin missing stream/hello");
        }
        ActiveSessionSlot* slot = nullptr;
        if (!command.session_id.empty()) {
            slot = find_by_session_id(command.session_id);
        }
        if (slot == nullptr) {
            if (command.is_reconnect) {
                slot = config_.hub->slot_for_reconnect(*command.hello);
            } else {
                slot = config_.hub->slot_for_late_viewer(*command.hello);
            }
        }
        if (slot == nullptr) {
            throw std::runtime_error("EnqueueJoin: no matching session");
        }
        slot->enqueue_join(
            std::move(*command.stream),
            std::move(*command.hello),
            command.is_reconnect);
        break;
    }
    case LobbyCommand::Kind::DestroySession:
        destroy_session(command.session_id, command.reason);
        break;
    case LobbyCommand::Kind::PauseForReconnect:
    case LobbyCommand::Kind::ResumeSession:
        // Wired when reconnect-hold policy moves fully into Lobby buckets.
        break;
    }
}

void SessionManager::start_session(SessionPlan plan, SessionId session_id) {
    std::lock_guard lock(mutex_);
    // Drop finished slots without taking mutex again (already held).
    for (auto& slot : slots_) {
        if (slot != nullptr && !slot->finished()) {
            const auto snap = slot->status_snapshot();
            if (snap.request_destroy_reason.has_value()) {
                slot->request_stop();
            }
        }
    }
    slots_.erase(
        std::remove_if(
            slots_.begin(),
            slots_.end(),
            [](const std::unique_ptr<ActiveSessionSlot>& slot) {
                if (slot == nullptr || !slot->finished()) {
                    return false;
                }
                slot->join();
                return true;
            }),
        slots_.end());

    if (!plan.save_username.empty() &&
        config_.hub->save_profile_active(plan.save_username)) {
        throw std::runtime_error(
            "user " + plan.save_username +
            " already has an active session; reconnect to it or end it first");
    }
    if (config_.hub->live_slot_count() >= config_.max_slots) {
        throw std::runtime_error("host is at max concurrent sessions");
    }
    if (!config_.host_config.ignore_controller.has_value()) {
        config_.host_config.ignore_controller = sdl_ignore_list_for_session(plan);
    }
    auto lease = SessionSlotLease::claim(
        kSlotLeaseSpan,
        parse_virtual_display_number(config_.host_config.virtual_display));
    if (!lease.valid()) {
        throw std::runtime_error(
            "no free session slot on this machine (another host is using them)");
    }
    auto launch_plan = launch_plan_for_session(plan);
    ActiveSessionSlotConfig slot_cfg;
    slot_cfg.slot_index = lease.index();
    slot_cfg.slot_lease = std::move(lease);
    slot_cfg.session_id = std::move(session_id);
    slot_cfg.host_config = config_.host_config;
    slot_cfg.plan = std::move(plan);
    slot_cfg.launch_plan = std::move(launch_plan);
    slot_cfg.catalog = config_.catalog;
    slot_cfg.game_list = config_.game_list;
    slot_cfg.hub = config_.hub;
    slot_cfg.input_demux = config_.input_demux;
    slot_cfg.streaming_audio = config_.streaming_audio;
    slot_cfg.bridge_device = config_.bridge_device;
    slot_cfg.should_stop = config_.should_stop;

    auto slot = std::make_unique<ActiveSessionSlot>(std::move(slot_cfg));
    std::cout
        << "Starting session " << slot->session_id()
        << " slot=" << slot->slot_index()
        << " mode=" << session_mode_name(slot->plan().session_mode)
        << " game=" << slot->plan().selected_game_id
        << " save_user=" << slot->plan().save_username << '\n';
    slot->start();
    slots_.push_back(std::move(slot));
}

void SessionManager::destroy_session(const SessionId& session_id, std::string_view reason) {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot == nullptr || slot->session_id() != session_id) {
            continue;
        }
        std::cout
            << "Destroying session " << session_id
            << " reason=" << reason << '\n';
        slot->request_stop();
        return;
    }
}

std::size_t SessionManager::reap_finished() {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot == nullptr || slot->finished()) {
            continue;
        }
        const auto snap = slot->status_snapshot();
        if (snap.request_destroy_reason.has_value()) {
            slot->request_stop();
        }
    }

    const auto before = slots_.size();
    slots_.erase(
        std::remove_if(
            slots_.begin(),
            slots_.end(),
            [](const std::unique_ptr<ActiveSessionSlot>& slot) {
                if (slot == nullptr || !slot->finished()) {
                    return false;
                }
                std::cout
                    << "Session " << slot->session_id()
                    << " slot=" << slot->slot_index()
                    << " finished; host lobby still accepting clients "
                       "(Stop Host to shut down).\n";
                slot->join();
                return true;
            }),
        slots_.end());
    if (slots_.size() != before) {
        (void)reap_stale_emulator_session_tokens();
    }
    return before - slots_.size();
}

void SessionManager::request_stop_all() {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot != nullptr) {
            slot->request_stop();
        }
    }
}

void SessionManager::join_all() {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot != nullptr) {
            slot->join();
        }
    }
    slots_.clear();
}

std::size_t SessionManager::live_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& slot : slots_) {
        if (slot != nullptr && !slot->finished()) {
            ++count;
        }
    }
    return count;
}

std::vector<SessionStatusSnapshot> SessionManager::statuses() const {
    std::lock_guard lock(mutex_);
    std::vector<SessionStatusSnapshot> out;
    out.reserve(slots_.size());
    for (const auto& slot : slots_) {
        if (slot != nullptr) {
            out.push_back(slot->status_snapshot());
        }
    }
    return out;
}

ActiveSessionSlot* SessionManager::find_by_session_id(const SessionId& session_id) {
    std::lock_guard lock(mutex_);
    for (auto& slot : slots_) {
        if (slot != nullptr && slot->session_id() == session_id) {
            return slot.get();
        }
    }
    return nullptr;
}

const ActiveSessionSlot* SessionManager::find_by_session_id(const SessionId& session_id) const {
    std::lock_guard lock(mutex_);
    for (const auto& slot : slots_) {
        if (slot != nullptr && slot->session_id() == session_id) {
            return slot.get();
        }
    }
    return nullptr;
}

} // namespace archstreamer
