#include "host/session/launch_types.hpp"

#include "common/steam_art_import.hpp"
#include "host/console/retroarch_config_writer.hpp"
#include "host/console/standalone_emulator.hpp"
#include "host/session/launch_assemble.hpp"
#include "host/session/lobby.hpp"
#include "host/video/capture_platform.hpp"

#include <stdexcept>
#include <utility>

namespace archstreamer {

SessionMediaPlan build_session_media_plan(
    const HostAppConfig& config,
    SessionPlan* session_plan,
    SessionMediaPlanKind kind) {
    SessionMediaPlan media;
    if (kind == SessionMediaPlanKind::Slot) {
        if (session_plan == nullptr) {
            throw std::invalid_argument("slot media plan requires a SessionPlan");
        }
        parse_video_resolution(config.media.video.resolution, media.capture_w, media.capture_h);
        configure_initial_session_video(*session_plan, media.capture_w, media.capture_h);
    }
    media.config = make_media_plan_config(config.media);
    if (kind == SessionMediaPlanKind::Slot) {
        media.config.initial_video_settings = session_plan->stream.video_settings;
    }
    if (config.media.video.enabled || config.media.audio.enabled) {
        if (kind == SessionMediaPlanKind::Slot) {
            media.destinations = media_destinations_for_session(media.config, *session_plan);
        } else {
            media.destinations = media_destinations_for_host(media.config);
        }
        media.streams = media_streams_for_dry_run(media.config, media.destinations);
    }
    return media;
}

SessionLaunchContext prepare_session_launch_context(
    HostAppConfig& config,
    GameCatalog& catalog,
    HostLaunchPlan launch_plan,
    std::string_view log_prefix) {
    auto context = SessionLaunchContext{};
    context.launch_plan = std::move(launch_plan);
    context.assets = prepare_session_launch_assets(
        config.save_root,
        config.pads,
        config.verbose,
        catalog,
        context.launch_plan.game_id,
        context.launch_plan.save_username,
        log_prefix);
    return context;
}

SessionParticipantContext resolve_direct_session_participants(
    std::string_view save_username) {
    SessionParticipantContext participants;
    participants.profile_display_name = preferred_steam_or_username_display_name(save_username);
    participants.display_layout = DisplayLayoutPreference::Auto;
    return participants;
}

SessionParticipantContext resolve_session_plan_participants(
    std::string_view save_username,
    const SessionPlan& plan) {
    SessionParticipantContext participants;
    participants.client_hellos.reserve(plan.clients.size());
    for (const auto& client : plan.clients) {
        participants.client_hellos.push_back(client.hello);
    }
    participants.profile_display_name = resolve_switch_profile_display_name(
        save_username,
        plan.host_hello,
        participants.client_hellos);
    participants.display_layout =
        resolve_display_layout_preference(plan.host_hello, participants.client_hellos);
    return participants;
}

PresentTarget resolve_session_present_target(
    HostAppConfig& config,
    const RetroArchLaunchConfig& launch_config) {
    return resolve_present_target_from_settings(
        config.media.video,
        launch_config.standalone,
        !launch_config.standalone &&
            core_needs_gl_on_virtual_display(launch_config.core_path));
}

void bind_session_plug(
    EmulatorLaunchEnvRequest& request,
    const PresentTarget& present,
    const HostAppConfig& config,
    std::string xtest_display) {
    request.plug.present = present;
    request.plug.present.xtest_display = std::move(xtest_display);
    request.plug.audio = config.media.audio.tap();
}

} // namespace archstreamer
